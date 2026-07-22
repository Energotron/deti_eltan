#include "ce_second_map_adapter.h"

#include <windows.h>

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* This adapter is compiled with clang/MinGW and calls directly into the
   game's Delphi/Borland-compiled code via raw inline-asm CALLs. Confirmed
   in practice (CEAdapterCreateSecondDestination raised a real
   EAccessViolation on turn 1 of a brand new game) that a fault deep inside
   such a call can corrupt memory outright rather than raising a script-
   level exception the RScript engine catches gracefully -- most likely
   because an internal Delphi exception tries to unwind through our foreign,
   non-Delphi stack frame. clang for this target does not support __try/
   __except (MSVC-only extension), so recover manually: a vectored
   exception handler plus setjmp/longjmp turns any crash during a guarded
   call into a safe "operation failed" return instead of an application
   crash. Verified against a real EXCEPTION_ACCESS_VIOLATION before wiring
   this into the risky galaxy/Con construction paths. */
static jmp_buf g_ce_recovery_point;
static volatile LONG g_ce_guard_active = 0;
static volatile LONG g_ce_veh_installed = 0;
static volatile LONG g_ce_fault_code = 0;
static volatile LONG g_ce_fault_eip = 0;
static volatile LONG g_ce_fault_access_type = -1;
static volatile LONG g_ce_fault_access_address = 0;

static LONG WINAPI ce_veh_handler(EXCEPTION_POINTERS *info) {
    DWORD code;
    if (InterlockedCompareExchange(&g_ce_guard_active, 0, 0) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_STACK_OVERFLOW) {
        InterlockedExchange(&g_ce_fault_code, (LONG)code);
        InterlockedExchange(&g_ce_fault_eip, (LONG)(uintptr_t)info->ExceptionRecord->ExceptionAddress);
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            info->ExceptionRecord->NumberParameters >= 2) {
            InterlockedExchange(&g_ce_fault_access_type,
                (LONG)info->ExceptionRecord->ExceptionInformation[0]);
            InterlockedExchange(&g_ce_fault_access_address,
                (LONG)info->ExceptionRecord->ExceptionInformation[1]);
        } else {
            InterlockedExchange(&g_ce_fault_access_type, -1);
            InterlockedExchange(&g_ce_fault_access_address, 0);
        }
        InterlockedExchange(&g_ce_guard_active, 0);
        longjmp(g_ce_recovery_point, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void ce_ensure_veh_installed(void) {
    if (InterlockedCompareExchange(&g_ce_veh_installed, 1, 0) == 0) {
        AddVectoredExceptionHandler(1, ce_veh_handler);
    }
}

static void ce_write_one_marker(const char *dir, const char *file_name, const char *payload, size_t length) {
    char marker_path[MAX_PATH];
    HANDLE file;
    DWORD written = 0;

    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\%s", dir, file_name) < 0) return;
    file = CreateFileA(marker_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, payload, (DWORD)length, &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void ce_write_text_marker(const char *file_name, const char *payload, size_t length) {
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];

    /* Redundant fixed-path copy: rules out any %TEMP% resolution mismatch
       between the game process and the tooling reading these markers back. */
    ce_write_one_marker("C:\\ce_debug", file_name, payload, length);

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return;
    ce_write_one_marker(marker_dir, file_name, payload, length);
}

static int ce_write_binary_file(
    const char *dir, const char *file_name, const void *payload, uint32_t length
) {
    char marker_path[MAX_PATH];
    HANDLE file;
    DWORD written = 0;

    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\%s", dir, file_name) < 0) return 0;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, length, &written, NULL) ||
        !FlushFileBuffers(file) || written != length) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return 1;
}

static void ce_write_progress(const char *label) {
    char payload[128];
    int size = snprintf(payload, sizeof(payload), "%s\r\n", label);
    if (size > 0) {
        ce_write_text_marker("con-build-progress.log", payload, (size_t)size);
    }
}

static void ce_write_fault_report(const char *label) {
    char payload[192];
    int size = snprintf(payload, sizeof(payload),
        "%s: code=0x%08lx eip=0x%08lx access_type=%ld access_addr=0x%08lx\r\n",
        label,
        (unsigned long)InterlockedCompareExchange(&g_ce_fault_code, 0, 0),
        (unsigned long)InterlockedCompareExchange(&g_ce_fault_eip, 0, 0),
        (long)InterlockedCompareExchange(&g_ce_fault_access_type, 0, 0),
        (unsigned long)InterlockedCompareExchange(&g_ce_fault_access_address, 0, 0));
    if (size > 0) {
        ce_write_text_marker("con-build-progress.log", payload, (size_t)size);
    }
}

/* Unconditional load marker: proves the engine actually mapped this DLL
   into its process, independent of whether any exported function is ever
   invoked from a Turn script. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        ce_write_progress("dllmain-process-attach");
    }
    return TRUE;
}

static volatile LONG g_ce_galaxy_ptr = 0;
static volatile LONG g_ce_fingerprint_hash = 0;
static volatile LONG g_ce_fingerprint_bytes = 0;
static volatile LONG g_ce_layout_written = 0;
static volatile LONG g_ce_layout_block_hash[4] = {0, 0, 0, 0};
static volatile LONG g_ce_layout_normalized_block_hash[4] = {0, 0, 0, 0};
static volatile LONG g_ce_layout_zero_mask_low = 0;
static volatile LONG g_ce_layout_zero_mask_high = 0;
static volatile LONG g_ce_layout_pointer_mask_low = 0;
static volatile LONG g_ce_layout_pointer_mask_high = 0;
static volatile LONG g_ce_layout_sample_bytes = 0;
static volatile LONG g_ce_layout_observation_tag = 0;
static volatile LONG g_ce_layout_observation_sequence = 0;
static volatile LONG g_ce_layout_observation_count = 0;
static volatile LONG g_ce_layout_observation_lock = 0;
static volatile LONG g_ce_layout_last_observation_tag = 0;
static volatile LONG g_ce_layout_output_latest = 0;
static volatile LONG g_ce_old_galaxy_ptr = 0;
static volatile LONG g_ce_second_galaxy_ptr = 0;
static volatile LONG g_ce_active_arm = 0;
static volatile LONG g_ce_native_switch_lock = 0;
static volatile LONG g_ce_second_generation_status = 0;
static volatile LONG g_ce_snapshot_lock = 0;
static volatile LONG g_ce_snapshot_done = 0;
static volatile LONG g_ce_save_hook_installed = 0;
static volatile LONG g_ce_save_hook_armed = 0;
static volatile LONG g_ce_save_hook_expected_galaxy = 0;
static void *g_ce_save_trampoline = NULL;
static volatile LONG g_ce_load_hook_installed = 0;
static volatile LONG g_ce_load_transform_armed = 0;
static volatile LONG g_ce_load_transform_seed = 0;
static void *g_ce_load_trampoline = NULL;
static volatile LONG g_ce_live_galaxy_ptr = 0;
static volatile LONG g_ce_live_seed = 0;
static volatile LONG g_ce_live_switch_lock = 0;
static HHOOK g_ce_keyboard_hook = NULL;
static ATOM g_ce_loading_window_class = 0;

/* Steam build 20648864 / Rangers.exe 2.1.2500.0 only. */
enum {
    CE_RANGERS_TIMESTAMP = 0x68eccc46u,
    CE_RANGERS_IMAGE_SIZE = 0x004d1000u,
    CE_RVA_GALAXY_IMPORT_CELL = 0x0048263cu,
    CE_RVA_TGALAXY_CLASS_CELL = 0x00438d90u,
    CE_RVA_TGALAXY_CONSTRUCTOR = 0x00439198u,
    CE_RVA_TGALAXY_INITIALIZE = 0x0043a034u,
    CE_RVA_TGALAXY_SAVE_TO_STREAM = 0x0043a1a4u,
    CE_RVA_TGALAXY_LOAD_FROM_STREAM = 0x0043b6ccu,
    CE_RVA_TGALAXY_GENERATE_STARS = 0x0044ff74u,
    CE_RVA_BUFFER_CLASS_CELL = 0x0042ea5cu,
    CE_RVA_BUFFER_CONSTRUCTOR = 0x0042eab0u,
    CE_RVA_TOBJECT_FREE = 0x000045acu,
    /* "Error in procedure TGalaxy.NextDay label = " sits in .text right
       before this prologue (VA 0x840f08); self in eax, a single boolean
       in dl (matches ce_call_delphi_method_byte's convention). */
    CE_RVA_TGALAXY_NEXTDAY = 0x00440f08u,
    /* TCon: the class TransferShip's "system" destination check accepts
       (found by disassembling the validator at VA 0x6403d9, which raises
       "TransferShip - invalid destination" unless the target IsA one of
       three classes; this is the first of the three, and the one
       TGalaxy.LoadFromStream constructs into [self+0x2c]). */
    CE_RVA_TCON_CLASS_CELL = 0x00438f6cu,
    CE_RVA_TCON_CONSTRUCTOR = 0x004482f8u,
    CE_RVA_LIST_ADD = 0x000161c0u,
    /* Globals TCon's own constructor (0x8482f8) touches: an optional
       "current context" object at CE_RVA_CON_CONTEXT_CELL (guarded by a
       null check in the engine's own code, so not necessarily a problem),
       and two sub-object class-ref cells it unconditionally constructs
       from without any null check. */
    CE_RVA_CON_CONTEXT_CELL = 0x0048c288u,
    CE_RVA_CON_SUBLIST_CLASS_CELL = 0x00471184u,
    CE_RVA_CON_SUBOBJ_CLASS_CELL = 0x00013f74u,
    /* Generic constructor trampoline (test dl; call [eax-0xc] i.e. virtual
       NewInstance) that TCon's own constructor calls 10 times, 6x for
       CE_RVA_CON_SUBLIST_CLASS_CELL and 4x for CE_RVA_CON_SUBOBJ_CLASS_CELL.
       Traced with tools/cfg_disasm.py: both classes share the same default
       NewInstance (VA 0x404544, itself unremarkable GetMem+InitInstance) --
       no per-class override, so nothing found in static analysis explains
       the repeat crash. Used to test each nested class in isolation. */
    CE_RVA_GENERIC_CTOR_TRAMPOLINE = 0x0000457cu,
    /* Raw record allocator (GetMem + zero-fill, not a Delphi constructor --
       no VMT set up) used by TGalaxy.LoadFromStream's header section to
       build [galaxy+0xc4], a list of 0x78-byte per-race records whose
       count comes from [galaxy+0x5c]. Untested hypothesis: TCon's
       constructor faults because our synthetic galaxy never has this list
       populated, and something in its ~6 nested sub-constructors expects
       to find it there. */
    CE_RVA_RAW_RECORD_ALLOC = 0x000067a0u,
    CE_GALAXY_RACE_LIST_COUNT_OFFSET = 0x5cu,
    CE_GALAXY_RACE_LIST_OFFSET = 0xc4u,
    CE_RACE_RECORD_BYTES = 0x78u
};

uint32_t CE_CALL CEAdapterAbiVersion(void) {
    return CE_ADAPTER_ABI_VERSION;
}

uint32_t CE_CALL CEAdapterCapabilities(void) {
    return CE_CAP_BIND_GALAXY_POINTER | CE_CAP_SMOKE_MARKER |
        CE_CAP_READONLY_GALAXY_FINGERPRINT |
        CE_CAP_READONLY_GALAXY_LAYOUT_SAMPLE |
        CE_CAP_READONLY_GALAXY_LAYOUT_LATEST |
        CE_CAP_POINTER_NORMALIZED_LAYOUT_HASH |
        CE_CAP_EXPERIMENTAL_ENGINE_GALAXY |
        CE_CAP_NATIVE_GALAXY_SNAPSHOT |
        CE_CAP_SAVE_LIFECYCLE_SNAPSHOT |
        CE_CAP_LOAD_LIFECYCLE_TRANSFORM |
        CE_CAP_LIVE_ARM_SWITCH;
}

uint32_t CE_CALL CEAdapterBindGalaxy(uint32_t galaxy_ptr) {
    if (galaxy_ptr == 0) {
        return 0;
    }
    InterlockedExchange(&g_ce_galaxy_ptr, (LONG)galaxy_ptr);
    return 1;
}

uint32_t CE_CALL CEAdapterGetBoundGalaxy(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_galaxy_ptr, 0, 0);
}

uint32_t CE_CALL CEAdapterEchoDword(uint32_t value) {
    return value;
}

uint32_t CE_CALL CEAdapterRunSmoke(uint32_t galaxy_ptr, uint32_t marker) {
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[256];
    HANDLE file;
    DWORD written = 0;
    int payload_size;

    if (galaxy_ptr == 0 || marker != 1128616787u) {
        return 0;
    }
    if (!CEAdapterBindGalaxy(galaxy_ptr)) {
        return 0;
    }
    if (GetTempPathA(MAX_PATH, temp_path) == 0) {
        return 0;
    }
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) {
        return 0;
    }
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    if (snprintf(marker_path, sizeof(marker_path), "%s\\adapter-smoke.json", marker_dir) < 0) {
        return 0;
    }
    payload_size = snprintf(
        payload,
        sizeof(payload),
        "{\"abi\":%u,\"capabilities\":%u,\"galaxy_ptr_nonzero\":true,\"marker\":%u}\r\n",
        CEAdapterAbiVersion(),
        CEAdapterCapabilities(),
        marker
    );
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) {
        return 0;
    }
    file = CreateFileA(
        marker_path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!WriteFile(file, payload, (DWORD)payload_size, &written, NULL)) {
        CloseHandle(file);
        return 0;
    }
    if (!FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return written == (DWORD)payload_size ? 1u : 0u;
}

static int ce_is_readable_protection(DWORD protect) {
    DWORD base = protect & 0xffu;
    if ((protect & PAGE_GUARD) != 0 || base == PAGE_NOACCESS) {
        return 0;
    }
    return base == PAGE_READONLY || base == PAGE_READWRITE ||
        base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

static uint32_t ce_fnv1a32(const unsigned char *data, uint32_t size) {
    uint32_t hash = 2166136261u;
    uint32_t index;
    for (index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 16777619u;
    }
    return hash;
}

uint32_t CE_CALL CEAdapterProbeGalaxy(uint32_t galaxy_ptr, uint32_t byte_count) {
    MEMORY_BASIC_INFORMATION memory;
    unsigned char sample[256];
    SIZE_T bytes_read = 0;
    uintptr_t address = (uintptr_t)galaxy_ptr;
    uintptr_t region_start;
    uintptr_t region_end;
    uint32_t hash;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[512];
    int payload_size;
    HANDLE file;
    DWORD written = 0;

    if (galaxy_ptr == 0 || byte_count < 16 || byte_count > sizeof(sample)) {
        return 0;
    }
    if (VirtualQuery((LPCVOID)address, &memory, sizeof(memory)) != sizeof(memory)) {
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    if (memory.State != MEM_COMMIT || !ce_is_readable_protection(memory.Protect) ||
        address < region_start || address > region_end ||
        byte_count > region_end - address) {
        return 0;
    }
    if (!ReadProcessMemory(
        GetCurrentProcess(), (LPCVOID)address, sample, byte_count, &bytes_read
    ) || bytes_read != byte_count) {
        return 0;
    }
    hash = ce_fnv1a32(sample, byte_count);
    InterlockedExchange(&g_ce_fingerprint_hash, (LONG)hash);
    InterlockedExchange(&g_ce_fingerprint_bytes, (LONG)byte_count);

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\galaxy-fingerprint.json", marker_dir) < 0) return 0;
    payload_size = snprintf(
        payload,
        sizeof(payload),
        "{\"abi\":%u,\"galaxy_ptr_nonzero\":true,\"sample_bytes\":%u,"
        "\"fnv1a32\":%u,\"region_size\":%lu,"
        "\"state\":%lu,\"protect\":%lu,\"type\":%lu,\"read_only\":true}\r\n",
        CEAdapterAbiVersion(), byte_count, hash, (unsigned long)memory.RegionSize,
        (unsigned long)memory.State, (unsigned long)memory.Protect,
        (unsigned long)memory.Type
    );
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) return 0;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)payload_size, &written, NULL) ||
        !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return written == (DWORD)payload_size ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterGetLastFingerprintHash(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_fingerprint_hash, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLastFingerprintBytes(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_fingerprint_bytes, 0, 0);
}

static int ce_is_readable_pointer(uint32_t candidate) {
    MEMORY_BASIC_INFORMATION memory;
    uintptr_t address = (uintptr_t)candidate;
    uintptr_t region_start;
    uintptr_t region_end;

    if (candidate < 0x10000u || (candidate & 3u) != 0) {
        return 0;
    }
    if (VirtualQuery((LPCVOID)address, &memory, sizeof(memory)) != sizeof(memory)) {
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    return memory.State == MEM_COMMIT && ce_is_readable_protection(memory.Protect) &&
        address >= region_start && address < region_end;
}

uint32_t CE_CALL CEAdapterSampleGalaxyLayout(uint32_t galaxy_ptr, uint32_t sample_tag) {
    unsigned char sample[256];
    unsigned char normalized[256];
    MEMORY_BASIC_INFORMATION memory;
    SIZE_T bytes_read = 0;
    uintptr_t address = (uintptr_t)galaxy_ptr;
    uintptr_t region_start;
    uintptr_t region_end;
    uint32_t block_hash[4];
    uint32_t normalized_hash[4];
    uint32_t zero_low = 0;
    uint32_t zero_high = 0;
    uint32_t pointer_low = 0;
    uint32_t pointer_high = 0;
    uint32_t index;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[1024];
    int payload_size;
    HANDLE file;
    DWORD written = 0;
    int latest_output;

    if (InterlockedCompareExchange(&g_ce_layout_written, 1, 0) != 0) {
        return 1;
    }
    if (galaxy_ptr == 0 ||
        VirtualQuery((LPCVOID)address, &memory, sizeof(memory)) != sizeof(memory)) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    if (memory.State != MEM_COMMIT || !ce_is_readable_protection(memory.Protect) ||
        address < region_start || address > region_end || sizeof(sample) > region_end - address ||
        !ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, sample,
            sizeof(sample), &bytes_read) || bytes_read != sizeof(sample)) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    memcpy(normalized, sample, sizeof(normalized));
    for (index = 0; index < 64; ++index) {
        uint32_t word;
        uint32_t bit = 1u << (index & 31u);
        memcpy(&word, sample + index * sizeof(word), sizeof(word));
        if (word == 0) {
            if (index < 32) zero_low |= bit; else zero_high |= bit;
        } else if (ce_is_readable_pointer(word)) {
            if (index < 32) pointer_low |= bit; else pointer_high |= bit;
            memset(normalized + index * sizeof(word), 0, sizeof(word));
        }
    }
    for (index = 0; index < 4; ++index) {
        block_hash[index] = ce_fnv1a32(sample + index * 64u, 64u);
        normalized_hash[index] = ce_fnv1a32(normalized + index * 64u, 64u);
        InterlockedExchange(&g_ce_layout_block_hash[index], (LONG)block_hash[index]);
        InterlockedExchange(
            &g_ce_layout_normalized_block_hash[index], (LONG)normalized_hash[index]
        );
    }
    InterlockedExchange(&g_ce_layout_zero_mask_low, (LONG)zero_low);
    InterlockedExchange(&g_ce_layout_zero_mask_high, (LONG)zero_high);
    InterlockedExchange(&g_ce_layout_pointer_mask_low, (LONG)pointer_low);
    InterlockedExchange(&g_ce_layout_pointer_mask_high, (LONG)pointer_high);
    InterlockedExchange(&g_ce_layout_sample_bytes, (LONG)sizeof(sample));

    latest_output = InterlockedCompareExchange(&g_ce_layout_output_latest, 0, 0) != 0;
    if (GetTempPathA(MAX_PATH, temp_path) == 0 ||
        snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0 ||
        (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        snprintf(
            marker_path,
            sizeof(marker_path),
            latest_output ? "%s\\galaxy-layout-latest.json" : "%s\\galaxy-layout-samples.jsonl",
            marker_dir
        ) < 0) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    payload_size = snprintf(
        payload,
        sizeof(payload),
        "{\"abi\":%u,\"process_id\":%lu,\"sample_tag\":%u,"
        "\"observation_tag\":%u,\"sequence\":%u,\"sample_bytes\":256,"
        "\"block_fnv1a32\":[%u,%u,%u,%u],\"zero_mask\":\"%08lX%08lX\","
        "\"pointer_normalized_fnv1a32\":[%u,%u,%u,%u],"
        "\"readable_pointer_mask\":\"%08lX%08lX\",\"raw_values_included\":false,"
        "\"read_only\":true}\r\n",
        CEAdapterAbiVersion(), (unsigned long)GetCurrentProcessId(), sample_tag,
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_tag, 0, 0),
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_sequence, 0, 0),
        block_hash[0], block_hash[1], block_hash[2], block_hash[3],
        (unsigned long)zero_high, (unsigned long)zero_low,
        normalized_hash[0], normalized_hash[1], normalized_hash[2], normalized_hash[3],
        (unsigned long)pointer_high, (unsigned long)pointer_low
    );
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    file = CreateFileA(
        marker_path,
        latest_output ? GENERIC_WRITE : FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        latest_output ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, payload, (DWORD)payload_size, &written, NULL) ||
        !FlushFileBuffers(file)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    CloseHandle(file);
    if (written != (DWORD)payload_size) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    return 1;
}

uint32_t CE_CALL CEAdapterObserveGalaxyLayout(
    uint32_t galaxy_ptr, uint32_t sample_tag, uint32_t observation_tag
) {
    uint32_t count;
    uint32_t result;

    if (galaxy_ptr == 0 || sample_tag == 0 ||
        InterlockedCompareExchange(&g_ce_layout_observation_lock, 1, 0) != 0) {
        return 0;
    }
    count = (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_count, 0, 0);
    if (count != 0 &&
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_last_observation_tag, 0, 0) ==
            observation_tag) {
        InterlockedExchange(&g_ce_layout_observation_lock, 0);
        return 1;
    }
    InterlockedExchange(&g_ce_layout_observation_tag, (LONG)observation_tag);
    InterlockedExchange(&g_ce_layout_observation_sequence, (LONG)(count + 1u));
    InterlockedExchange(&g_ce_layout_output_latest, 1);
    InterlockedExchange(&g_ce_layout_written, 0);
    result = CEAdapterSampleGalaxyLayout(galaxy_ptr, sample_tag);
    InterlockedExchange(&g_ce_layout_output_latest, 0);
    if (result != 0) {
        InterlockedExchange(&g_ce_layout_observation_count, (LONG)(count + 1u));
        InterlockedExchange(&g_ce_layout_last_observation_tag, (LONG)observation_tag);
    }
    InterlockedExchange(&g_ce_layout_observation_lock, 0);
    return result;
}

uint32_t CE_CALL CEAdapterGetLayoutObservationCount(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_count, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutBlockHash(uint32_t block_index) {
    if (block_index >= 4) return 0;
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_block_hash[block_index], 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutNormalizedBlockHash(uint32_t block_index) {
    if (block_index >= 4) return 0;
    return (uint32_t)InterlockedCompareExchange(
        &g_ce_layout_normalized_block_hash[block_index], 0, 0
    );
}

uint32_t CE_CALL CEAdapterGetLayoutZeroMaskLow(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_zero_mask_low, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutZeroMaskHigh(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_zero_mask_high, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutReadablePointerMaskLow(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_pointer_mask_low, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutReadablePointerMaskHigh(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_pointer_mask_high, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutSampleBytes(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_sample_bytes, 0, 0);
}

uint32_t CE_CALL CEAdapterSupportsNativeMultiGalaxy(void) {
    return InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0) != 0 ? 1u : 0u;
}

static int ce_region_has_access(const void *pointer, size_t bytes, int need_write) {
    MEMORY_BASIC_INFORMATION memory;
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t region_start;
    uintptr_t region_end;
    DWORD base;

    if (pointer == NULL || bytes == 0 ||
        VirtualQuery(pointer, &memory, sizeof(memory)) != sizeof(memory)) {
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    base = memory.Protect & 0xffu;
    if (memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 ||
        address < region_start || address > region_end || bytes > region_end - address) {
        return 0;
    }
    if (need_write) {
        return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
            base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    }
    return ce_is_readable_protection(memory.Protect);
}

static int ce_resolve_engine_galaxy(
    uint32_t galaxy_ptr,
    uintptr_t *module_base_out,
    uint32_t **galaxy_slot_out,
    uint32_t *class_ref_out
) {
    static const unsigned char constructor_signature[] = {
        0x55, 0x8b, 0xec, 0xb9, 0x0a, 0x00, 0x00, 0x00,
        0x6a, 0x00, 0x6a, 0x00, 0x49, 0x75, 0xf9, 0x51
    };
    static const unsigned char initializer_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xf8, 0x89, 0x45,
        0xfc, 0x8b, 0x45, 0xfc, 0x33, 0xd2, 0x89, 0x50
    };
    static const unsigned char generator_signature[] = {
        0x55, 0x8b, 0xec, 0x81, 0xc4, 0x6c, 0xfe, 0xff,
        0xff, 0x53, 0x56, 0x57, 0x33, 0xc9, 0x89, 0x4d
    };
    HMODULE module = GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS32 *nt;
    uintptr_t base;
    uint32_t **import_cell;
    uint32_t *slot;
    uint32_t class_ref;

    if (module == NULL || galaxy_ptr == 0) return 0;
    base = (uintptr_t)module;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (!ce_region_has_access(dos, sizeof(*dos), 0) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS32 *)(base + (uintptr_t)dos->e_lfanew);
    if (!ce_region_has_access(nt, sizeof(*nt), 0) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != CE_RANGERS_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != CE_RANGERS_IMAGE_SIZE) {
        return 0;
    }
    if (memcmp((const void *)(base + CE_RVA_TGALAXY_CONSTRUCTOR),
            constructor_signature, sizeof(constructor_signature)) != 0 ||
        memcmp((const void *)(base + CE_RVA_TGALAXY_INITIALIZE),
            initializer_signature, sizeof(initializer_signature)) != 0 ||
        memcmp((const void *)(base + CE_RVA_TGALAXY_GENERATE_STARS),
            generator_signature, sizeof(generator_signature)) != 0) {
        return 0;
    }
    import_cell = (uint32_t **)(base + CE_RVA_GALAXY_IMPORT_CELL);
    if (!ce_region_has_access(import_cell, sizeof(*import_cell), 0)) return 0;
    slot = *import_cell;
    if (!ce_region_has_access(slot, sizeof(*slot), 1) || *slot != galaxy_ptr ||
        !ce_region_has_access((const void *)(uintptr_t)galaxy_ptr, 0x1dcu, 0)) {
        return 0;
    }
    class_ref = *(const uint32_t *)(base + CE_RVA_TGALAXY_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)class_ref, sizeof(uint32_t), 0) ||
        *(const uint32_t *)(uintptr_t)galaxy_ptr != class_ref) {
        return 0;
    }
    *module_base_out = base;
    *galaxy_slot_out = slot;
    *class_ref_out = class_ref;
    return 1;
}

static uint32_t ce_call_delphi_constructor(uint32_t class_ref, uintptr_t function_address) {
    uint32_t result;
    __asm__ volatile(
        "movb $1, %%dl\n\t"
        "movl %1, %%eax\n\t"
        "call *%2\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(class_ref), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static void ce_call_delphi_method(uint32_t self, uintptr_t function_address) {
    __asm__ volatile(
        "movl %0, %%eax\n\t"
        "call *%1"
        :
        : "r"(self), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
}

static void ce_call_delphi_method_byte(
    uint32_t self, uint32_t byte_argument, uintptr_t function_address
) {
    __asm__ volatile(
        "movb %b1, %%dl\n\t"
        "movl %0, %%eax\n\t"
        "call *%2"
        :
        : "r"(self), "q"(byte_argument), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
}

static uint32_t ce_call_delphi_method_dword(
    uint32_t self, uint32_t dword_argument, uintptr_t function_address
) {
    uint32_t result;
    __asm__ volatile(
        "movl %1, %%edx\n\t"
        "movl %2, %%eax\n\t"
        "call *%3\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(dword_argument), "r"(self), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static uint32_t ce_call_raw_alloc(uint32_t size, uintptr_t function_address) {
    uint32_t result;
    __asm__ volatile(
        "movl %1, %%eax\n\t"
        "call *%2\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(size), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static void ce_write_native_stage(uint32_t stage, uint32_t star_count) {
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[160];
    HANDLE file;
    DWORD written = 0;
    int payload_size;

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\native-second-galaxy.json", marker_dir) < 0) return;
    payload_size = snprintf(payload, sizeof(payload),
        "{\"abi\":%u,\"stage\":%u,\"stars\":%u}\r\n",
        CEAdapterAbiVersion(), stage, star_count);
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) return;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, payload, (DWORD)payload_size, &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

/* Dev-only diagnostic: dumps raw dwords [0x00, 0x200) of galaxy_ptr so two
   snapshots (a known-working real galaxy vs. the synthetic second one) can
   be diffed offline to find which field GalaxyStars() actually reads. Not
   part of the read-only fingerprint capability (that one intentionally
   never emits raw values); this is throwaway spike tooling. */
uint32_t CE_CALL CEAdapterDumpGalaxyWords(uint32_t galaxy_ptr, uint32_t tag) {
    static const uint32_t dump_bytes = 0x200u;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[4096];
    int offset = 0;
    int written_chars;
    uint32_t index;
    HANDLE file;
    DWORD written = 0;

    if (galaxy_ptr == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)galaxy_ptr, dump_bytes, 0)) {
        return 0;
    }
    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\galaxy-dump-%u.json", marker_dir, tag) < 0) {
        return 0;
    }
    written_chars = snprintf(payload, sizeof(payload), "{\"abi\":%u,\"tag\":%u,\"words\":[",
        CEAdapterAbiVersion(), tag);
    if (written_chars <= 0) return 0;
    offset = written_chars;
    for (index = 0; index < dump_bytes / 4u; ++index) {
        uint32_t value = *(const uint32_t *)(uintptr_t)(galaxy_ptr + index * 4u);
        written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset,
            index == 0 ? "%u" : ",%u", value);
        if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
        offset += written_chars;
    }
    written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset, "]}\r\n");
    if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
    offset += written_chars;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)offset, &written, NULL) || !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return written == (DWORD)offset ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterProbeEngineGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    return ce_resolve_engine_galaxy(
        galaxy_ptr, &module_base, &galaxy_slot, &class_ref
    ) ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterCreateAndEnterSecondGalaxy(
    uint32_t galaxy_ptr, uint32_t second_seed, uint32_t player_race
) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t second;
    uint32_t star_count = 0;
    uint32_t star_list;

    if (second_seed == 0 || player_race > 7u ||
        InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) {
        return 0;
    }
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    second = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second != 0) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 2u;
    }

    second = ce_call_delphi_constructor(
        class_ref, module_base + CE_RVA_TGALAXY_CONSTRUCTOR
    );
    if (second == 0 || !ce_region_has_access((const void *)(uintptr_t)second, 0x1dcu, 1) ||
        *(const uint32_t *)(uintptr_t)second != class_ref) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }

    memcpy((void *)(uintptr_t)(second + 0x50u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x50u), 0x10u);
    memcpy((void *)(uintptr_t)(second + 0x188u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x188u), 0x27u);
    /* CE_RVA_TGALAXY_GENERATE_STARS (disassembled from the installed exe)
       reads its star-count target from [self+0x160] and `jle`-skips the
       whole generation loop when it is <= 0. The raw Delphi constructor
       call never sets it, so the field is zero on a synthetic instance;
       copy the real galaxy's target so the second arm generates the same
       number of stars. */
    memcpy((void *)(uintptr_t)(second + 0x160u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x160u), 0x4u);
    *(uint32_t *)(uintptr_t)(second + 0x1d4u) = second_seed;

    InterlockedExchange(&g_ce_old_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_second_generation_status, 1);
    ce_write_native_stage(1u, 0u);

    /* Runs synchronously on the caller (engine main) thread: the engine's
       Initialize/GenerateStars are not thread-safe, and the global Galaxy
       slot must only be swapped while no other engine code can observe it. */
    *galaxy_slot = second;
    ce_call_delphi_method(second, module_base + CE_RVA_TGALAXY_INITIALIZE);
    ce_write_native_stage(3u, 0u);
    ce_call_delphi_method_byte(second, player_race,
        module_base + CE_RVA_TGALAXY_GENERATE_STARS);
    /* GenerateStars clears and fills the list at [self+0x164] (a TList
       subclass: Clear() via its vtable, Add() per created star), not the
       field at +0x2c the previous ABI 8 build read back from; +0x2c is a
       different, unrelated pointer and always read back as empty. */
    star_list = *(uint32_t *)(uintptr_t)(second + 0x164u);
    if (ce_region_has_access((const void *)(uintptr_t)star_list, 12u, 0)) {
        star_count = *(uint32_t *)(uintptr_t)(star_list + 8u);
    }
    *galaxy_slot = galaxy_ptr;
    if (star_count != 0u) {
        InterlockedExchange(&g_ce_second_galaxy_ptr, (LONG)second);
        InterlockedExchange(&g_ce_second_generation_status, 2);
        ce_write_native_stage(4u, star_count);
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 1;
    }
    /* Leave status at 0 (not a permanent 3) so the next day-skip retries
       instead of going silent forever; the failed `second` instance is
       abandoned (small leak, acceptable for this dev-only smoke module). */
    InterlockedExchange(&g_ce_second_generation_status, 0);
    ce_write_native_stage(5u, 0u);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 0;
}

uint32_t CE_CALL CEAdapterSecondGalaxyStatus(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_second_generation_status, 0, 0);
}

uint32_t CE_CALL CEAdapterEnterReadySecondGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t old_galaxy;
    uint32_t second_galaxy;

    if (InterlockedCompareExchange(&g_ce_second_generation_status, 0, 0) != 2 ||
        InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (galaxy_ptr != old_galaxy || second_galaxy == 0 ||
        !ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        ce_write_progress("enter-ready:validation-failed");
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    ce_write_progress("enter-ready:before-swap");
    *galaxy_slot = second_galaxy;
    InterlockedExchange(&g_ce_active_arm, 1);
    ce_write_progress("enter-ready:after-swap");
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

uint32_t CE_CALL CEAdapterReturnToOldGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t old_galaxy;
    uint32_t second_galaxy;

    if (InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 || old_galaxy == 0 || galaxy_ptr != second_galaxy ||
        !ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    *galaxy_slot = old_galaxy;
    InterlockedExchange(&g_ce_active_arm, 0);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

uint32_t CE_CALL CEAdapterActiveArm(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_active_arm, 0, 0);
}

/* CEAdapterEnterReadySecondGalaxy (*galaxy_slot = second_galaxy, then back)
   twice reproduced a Rangers.exe crash in TGalaxy.NextDay right after being
   exercised in-game -- once after an empty-galaxy round trip, once even on
   a fresh save on the very next day-skip. The cause is not understood yet
   (TGalaxy.NextDay is not one of the three methods this adapter calls), so
   CE_MapSmoke stops calling Enter/Return/Abandon entirely and only marks
   the attempt terminal through this no-pointer-touching call: no engine
   memory is written, nothing is switched, only the internal status flag
   moves so the Turn code stops retrying. */
uint32_t CE_CALL CEAdapterMarkSecondGalaxyEntryDisabled(void) {
    InterlockedExchange(&g_ce_second_generation_status, 4);
    return 1;
}

/* RScript's own GalaxyStars()/GalaxyStar() read a count the post-generation
   orchestrator (name assignment, sectors, economy -- not yet reproduced
   here) fills in, so they report 0 for a galaxy GenerateStars alone built.
   The list GenerateStars actually Clear()s/Add()s into at [self+0x164] is a
   plain Delphi TList (FList ptr @+4, FCount @+8) and its entries are real
   star-class instances (built via the same class GenerateStars uses for
   every star it creates), so read that list directly instead of going
   through the not-yet-populated engine-level accessors. */
static uint32_t ce_read_generated_star_list(
    uint32_t galaxy_ptr, uint32_t *list_out, uint32_t *count_out
) {
    uint32_t star_list;
    uint32_t count;

    if (galaxy_ptr == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)galaxy_ptr, 0x168u, 0)) {
        return 0;
    }
    star_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x164u);
    if (!ce_region_has_access((const void *)(uintptr_t)star_list, 12u, 0)) {
        return 0;
    }
    count = *(const uint32_t *)(uintptr_t)(star_list + 8u);
    *list_out = star_list;
    *count_out = count;
    return 1;
}

uint32_t CE_CALL CEAdapterGetGeneratedStarCount(uint32_t galaxy_ptr) {
    uint32_t star_list;
    uint32_t count;
    if (!ce_read_generated_star_list(galaxy_ptr, &star_list, &count)) return 0;
    return count;
}

uint32_t CE_CALL CEAdapterGetGeneratedStarByIndex(uint32_t galaxy_ptr, uint32_t index) {
    uint32_t star_list;
    uint32_t count;
    uint32_t array_ptr;

    if (!ce_read_generated_star_list(galaxy_ptr, &star_list, &count) || index >= count) {
        return 0;
    }
    array_ptr = *(const uint32_t *)(uintptr_t)(star_list + 4u);
    if (!ce_region_has_access((const void *)(uintptr_t)array_ptr, (index + 1u) * 4u, 0)) {
        return 0;
    }
    return *(const uint32_t *)(uintptr_t)(array_ptr + index * 4u);
}

/* TransferShip only accepts a "system"/"planet"/"station" instance as its
   destination (three IsA checks against fixed class references; see the
   validator at VA 0x6403d9). None of the classes GenerateStars builds pass
   that check, which is exactly the "invalid destination" error the real
   game raised in-game. Rather than reproduce the full LoadFromStream
   deserializer that would normally populate real, fully-formed instances
   of the first class (TCon, at [galaxy+0x2c]), construct one bare instance
   directly via the engine's own constructor -- it passes the IsA check
   regardless of whether its other fields are populated -- and copy a
   position from the real galaxy's own first Con so it does not render at
   (0,0). Untested whether downstream code (map rendering, sector lookup)
   tolerates the otherwise-blank instance; that is the next unknown. */
/* Pure read-only diagnostic: reports whether the TCon class-ref cell looks
   like a valid pointer yet, without ever constructing anything. Written
   after CEAdapterCreateSecondDestination crashed for real on turn 1 of a
   brand new game -- need to know whether/when this cell settles before
   trying construction again. Safe to call every turn. */
static uint32_t ce_probe_cell(uintptr_t cell_address, uint32_t *value_out) {
    uint32_t value;
    if (!ce_region_has_access((const void *)cell_address, 4u, 0)) {
        *value_out = 0;
        return 0;
    }
    value = *(const uint32_t *)cell_address;
    *value_out = value;
    return value != 0 && ce_region_has_access((const void *)(uintptr_t)value, 4u, 0);
}

uint32_t CE_CALL CEAdapterProbeConClass(uint32_t old_galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref, con_class_ok;
    uint32_t context_value, context_ok;
    uint32_t sublist_class_ref, sublist_ok;
    uint32_t subobj_class_ref, subobj_ok;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[400];
    int payload_size;
    HANDLE file;
    DWORD written = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    con_class_ok = ce_probe_cell(module_base + CE_RVA_TCON_CLASS_CELL, &con_class_ref);
    context_ok = ce_probe_cell(module_base + CE_RVA_CON_CONTEXT_CELL, &context_value);
    sublist_ok = ce_probe_cell(module_base + CE_RVA_CON_SUBLIST_CLASS_CELL, &sublist_class_ref);
    subobj_ok = ce_probe_cell(module_base + CE_RVA_CON_SUBOBJ_CLASS_CELL, &subobj_class_ref);

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\con-class-probe.jsonl", marker_dir) < 0) {
        return 0;
    }
    payload_size = snprintf(payload, sizeof(payload),
        "{\"turn\":%u,\"con_class_ref\":%u,\"con_class_ok\":%s,"
        "\"context_value\":%u,\"context_ok\":%s,"
        "\"sublist_class_ref\":%u,\"sublist_ok\":%s,"
        "\"subobj_class_ref\":%u,\"subobj_ok\":%s}\r\n",
        turn, con_class_ref, con_class_ok ? "true" : "false",
        context_value, context_ok ? "true" : "false",
        sublist_class_ref, sublist_ok ? "true" : "false",
        subobj_class_ref, subobj_ok ? "true" : "false");
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) return 0;
    file = CreateFileA(marker_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)payload_size, &written, NULL) || !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return (con_class_ok && sublist_ok && subobj_ok) ? 1u : 0u;
}

/* [0x882580] is a pointer-to-int used throughout TGalaxy.LoadFromStream /
   TCon's nested loader as a save-format version gate (compared against
   thresholds like 0x65, 0x7a, 0x85, 0x9e as the format grew over time).
   Need the exact runtime value once: guessing wrong about which side of a
   version gate we're on shifts every subsequent stream read by however
   many bytes that branch reads, corrupting the entire rest of a
   hand-built buffer. Pure read-only, writes nothing, safe every turn. */
uint32_t CE_CALL CEAdapterProbeSaveFormatVersion(uint32_t old_galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t version_cell_ok, version_ptr, version_ptr_ok, version_value;
    char payload[128];
    int payload_size;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    version_cell_ok = ce_probe_cell(module_base + 0x00482580u, &version_ptr);
    version_ptr_ok = version_cell_ok &&
        ce_region_has_access((const void *)(uintptr_t)version_ptr, 4u, 0);
    version_value = version_ptr_ok ? *(const uint32_t *)(uintptr_t)version_ptr : 0;

    payload_size = snprintf(payload, sizeof(payload),
        "{\"turn\":%u,\"version_ptr_ok\":%s,\"version_value\":%u}\r\n",
        turn, version_ptr_ok ? "true" : "false", version_value);
    if (payload_size > 0) {
        ce_write_text_marker("save-format-version.jsonl", payload, (size_t)payload_size);
    }
    return version_ptr_ok ? 1u : 0u;
}

/* Untested hypothesis for why TCon's constructor keeps corrupting shared
   state regardless of which individual side effect gets patched around:
   our synthetic galaxy never has [galaxy+0xc4] (a list of 0x78-byte
   per-race records, count at [galaxy+0x5c]) populated the way
   TGalaxy.LoadFromStream's header section builds it on a real load, and
   something in TCon's ~6 nested sub-constructors may expect to find it
   there. [galaxy+0xc4]'s records are built via a raw GetMem-style
   allocator (VA 0x4067a0), not a Delphi constructor -- no VMT to set up --
   so cloning them from the real galaxy is a plain, low-risk memcpy per
   record, guarded the same way as the rest of this file. Called once
   before ever attempting Con construction. */
/* Isolated test for which (if either) of TCon's two nested sub-object
   classes faults when constructed alone, outside TCon's constructor
   entirely -- narrows down whether the earlier repeat crash is really
   about one specific class or about the calling context in general.
   Each attempt is independently SEH-guarded so one faulting doesn't stop
   the other from being tried, and progress is logged before/after each. */
uint32_t CE_CALL CEAdapterProbeSubobjectConstruction(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t sublist_class_ref, subobj_class_ref;
    uint32_t sublist_result = 0, subobj_result = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("subobj-probe-abort:resolve-failed");
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_CON_SUBLIST_CLASS_CELL), 4u, 0) ||
        !ce_region_has_access((const void *)(module_base + CE_RVA_CON_SUBOBJ_CLASS_CELL), 4u, 0)) {
        ce_write_progress("subobj-probe-abort:class-cells-unreadable");
        return 0;
    }
    sublist_class_ref = *(const uint32_t *)(module_base + CE_RVA_CON_SUBLIST_CLASS_CELL);
    subobj_class_ref = *(const uint32_t *)(module_base + CE_RVA_CON_SUBOBJ_CLASS_CELL);

    ce_ensure_veh_installed();

    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("subobj-probe:sublist-class-FAULTED");
    } else {
        InterlockedExchange(&g_ce_guard_active, 1);
        ce_write_progress("subobj-probe:before-sublist-class");
        sublist_result = ce_call_delphi_constructor(
            sublist_class_ref, module_base + CE_RVA_GENERIC_CTOR_TRAMPOLINE);
        InterlockedExchange(&g_ce_guard_active, 0);
        ce_write_progress(sublist_result != 0
            ? "subobj-probe:sublist-class-ok"
            : "subobj-probe:sublist-class-returned-null");
    }

    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("subobj-probe:subobj-class-FAULTED");
    } else {
        InterlockedExchange(&g_ce_guard_active, 1);
        ce_write_progress("subobj-probe:before-subobj-class");
        subobj_result = ce_call_delphi_constructor(
            subobj_class_ref, module_base + CE_RVA_GENERIC_CTOR_TRAMPOLINE);
        InterlockedExchange(&g_ce_guard_active, 0);
        ce_write_progress(subobj_result != 0
            ? "subobj-probe:subobj-class-ok"
            : "subobj-probe:subobj-class-returned-null");
    }

    return (sublist_result != 0 ? 1u : 0u) | (subobj_result != 0 ? 2u : 0u);
}

/* Both of TCon's nested classes now confirmed safe to construct alone
   (CEAdapterProbeSubobjectConstruction). CEAdapterCreateSecondDestination
   has never once logged "after-construct" -- every attempt faults inside
   TCon's own constructor (VA 0x8482f8) before returning. This isolates
   the very first thing that constructor does: allocate a bare TCon-sized
   instance via the *generic* trampoline (same one used for the nested
   classes) instead of TCon's own full constructor body, skipping all 10
   nested constructions and every field initializer. If this succeeds,
   the fault is somewhere in TCon's constructor logic itself, not in the
   base allocation. */
uint32_t CE_CALL CEAdapterProbeBareConAllocation(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref;
    uint32_t result = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("bare-con-abort:resolve-failed");
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_TCON_CLASS_CELL), 4u, 0)) {
        ce_write_progress("bare-con-abort:class-cell-unreadable");
        return 0;
    }
    con_class_ref = *(const uint32_t *)(module_base + CE_RVA_TCON_CLASS_CELL);

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("bare-con-probe:FAULTED");
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);
    ce_write_progress("bare-con-probe:before");
    result = ce_call_delphi_constructor(
        con_class_ref, module_base + CE_RVA_GENERIC_CTOR_TRAMPOLINE);
    InterlockedExchange(&g_ce_guard_active, 0);
    ce_write_progress(result != 0 ? "bare-con-probe:ok" : "bare-con-probe:returned-null");
    return result;
}

/* The static (on-disk) value of the TCon class-ref cell is 0x00838fb8, a
   normal in-module VMT address -- but the *runtime* value read by every
   probe so far has been a heap address (order of 0x0c400000), nowhere near
   the module's mapped range (0x400000-0x8d1000). Something patches this
   cell at runtime, presumably a DLC/plugin-style VMT clone-and-override --
   a known Delphi pattern. Dump raw dwords around the runtime VMT pointer
   (covering the usual negative-offset VMT slot range, matching Delphi's
   classic layout) so they can be diffed against the static file bytes at
   the original address offline, to find exactly which slot differs. */
uint32_t CE_CALL CEAdapterDumpConVmt(uint32_t old_galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[2048];
    int offset;
    int written_chars;
    int32_t index;
    HANDLE file;
    DWORD written = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_TCON_CLASS_CELL), 4u, 0)) {
        return 0;
    }
    con_class_ref = *(const uint32_t *)(module_base + CE_RVA_TCON_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)(con_class_ref - 0x60u), 0x80u, 0)) {
        return 0;
    }

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\con-vmt-dump.json", marker_dir) < 0) {
        return 0;
    }
    written_chars = snprintf(payload, sizeof(payload),
        "{\"turn\":%u,\"con_class_ref\":%u,\"words\":[", turn, con_class_ref);
    if (written_chars <= 0) return 0;
    offset = written_chars;
    /* Offsets -0x60..+0x1c relative to the class ref, 4 bytes at a time:
       covers the classic Delphi VMT negative slot table plus a little
       past the pointer itself. */
    for (index = -0x60; index <= 0x1c; index += 4) {
        uint32_t value = *(const uint32_t *)(uintptr_t)((int32_t)con_class_ref + index);
        written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset,
            index == -0x60 ? "%u" : ",%u", value);
        if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
        offset += written_chars;
    }
    written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset, "]}\r\n");
    if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
    offset += written_chars;

    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)offset, &written, NULL) || !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return written == (DWORD)offset ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterCloneRaceRecords(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t second_galaxy;
    uint32_t old_count;
    uint32_t old_list, old_array;
    uint32_t new_list;
    uint32_t index;
    uint32_t cloned = 0;

    if (old_galaxy_ptr == 0) {
        ce_write_progress("race-clone-abort:no-old-galaxy");
        return 0;
    }
    if (!ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("race-clone-abort:resolve-failed");
        return 0;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0) {
        ce_write_progress("race-clone-abort:no-second-galaxy");
        return 0;
    }
    if (!ce_region_has_access((const void *)(uintptr_t)second_galaxy, CE_GALAXY_RACE_LIST_OFFSET + 4u, 1) ||
        !ce_region_has_access((const void *)(uintptr_t)old_galaxy_ptr, CE_GALAXY_RACE_LIST_OFFSET + 4u, 0)) {
        ce_write_progress("race-clone-abort:base-fields-unreadable");
        return 0;
    }

    old_count = *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + CE_GALAXY_RACE_LIST_COUNT_OFFSET);
    old_list = *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + CE_GALAXY_RACE_LIST_OFFSET);
    new_list = *(const uint32_t *)(uintptr_t)(second_galaxy + CE_GALAXY_RACE_LIST_OFFSET);
    {
        char diag[160];
        int diag_size = snprintf(diag, sizeof(diag),
            "race-clone-fields old_count=%u old_list=%u new_list=%u",
            old_count, old_list, new_list);
        if (diag_size > 0) ce_write_progress(diag);
    }
    if (old_count == 0u) {
        ce_write_progress("race-clone-abort:old_count-is-zero");
        return 0;
    }
    if (!ce_region_has_access((const void *)(uintptr_t)old_list, 12u, 0)) {
        ce_write_progress("race-clone-abort:old_list-unreadable");
        return 0;
    }
    if (!ce_region_has_access((const void *)(uintptr_t)new_list, 8u, 0)) {
        ce_write_progress("race-clone-abort:new_list-unreadable");
        return 0;
    }
    if (*(const uint32_t *)(uintptr_t)(old_list + 8u) < old_count) {
        ce_write_progress("race-clone-abort:old_list-count-mismatch");
        return 0;
    }
    old_array = *(const uint32_t *)(uintptr_t)(old_list + 4u);
    if (!ce_region_has_access((const void *)(uintptr_t)old_array, old_count * 4u, 0)) {
        ce_write_progress("race-clone-abort:old_array-unreadable");
        return 0;
    }

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("race-clone-recovered-from-fault");
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);

    for (index = 0; index < old_count; ++index) {
        uint32_t old_record = *(const uint32_t *)(uintptr_t)(old_array + index * 4u);
        uint32_t new_record;
        if (!ce_region_has_access((const void *)(uintptr_t)old_record, CE_RACE_RECORD_BYTES, 0)) {
            continue;
        }
        new_record = ce_call_raw_alloc(CE_RACE_RECORD_BYTES, module_base + CE_RVA_RAW_RECORD_ALLOC);
        if (new_record == 0 ||
            !ce_region_has_access((const void *)(uintptr_t)new_record, CE_RACE_RECORD_BYTES, 1)) {
            continue;
        }
        memcpy((void *)(uintptr_t)new_record, (const void *)(uintptr_t)old_record, CE_RACE_RECORD_BYTES);
        ce_call_delphi_method_dword(new_list, new_record, module_base + CE_RVA_LIST_ADD);
        ++cloned;
    }
    InterlockedExchange(&g_ce_guard_active, 0);
    {
        char summary[64];
        int summary_size = snprintf(summary, sizeof(summary),
            "race-clone-done cloned=%u of %u", cloned, old_count);
        if (summary_size > 0) ce_write_progress(summary);
    }

    if (cloned == old_count &&
        ce_region_has_access((const void *)(uintptr_t)second_galaxy, CE_GALAXY_RACE_LIST_COUNT_OFFSET + 4u, 1)) {
        *(uint32_t *)(uintptr_t)(second_galaxy + 0x58u) =
            *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + 0x58u);
        *(uint32_t *)(uintptr_t)(second_galaxy + CE_GALAXY_RACE_LIST_COUNT_OFFSET) = old_count;
    }
    return cloned;
}

uint32_t CE_CALL CEAdapterCreateSecondDestination(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref;
    uint32_t second_galaxy;
    uint32_t con_list;
    uint32_t new_con;
    uint32_t ref_list;
    uint32_t ref_count;
    uint32_t ref_array;
    uint32_t ref_con;
    float ref_x;
    float ref_y;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)second_galaxy, 0x30u, 0)) {
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_TCON_CLASS_CELL), 4u, 0)) {
        return 0;
    }
    con_class_ref = *(const uint32_t *)(module_base + CE_RVA_TCON_CLASS_CELL);
    /* The class-ref cell crashed the constructor on day 1 of a brand new
       game (write access violation inside the engine's own NewInstance),
       most likely because this global isn't populated that early in the
       engine's own bootstrap yet. Require it to at least look like a
       plausible readable pointer before ever calling into the constructor
       with it -- this alone would have turned that crash into a safe 0
       return instead of a corrupted write. */
    if (!ce_region_has_access((const void *)(uintptr_t)con_class_ref, 4u, 0)) {
        return 0;
    }

    con_list = *(const uint32_t *)(uintptr_t)(second_galaxy + 0x2cu);
    if (!ce_region_has_access((const void *)(uintptr_t)con_list, 12u, 0)) {
        return 0;
    }

    /* Root cause of the earlier real corruption (confirmed by disassembling
       TCon's constructor, VA 0x8482f8): when [0x88c288] (a "current
       context" global, non-null during normal play per
       CEAdapterProbeConClass's own field) is non-null, the constructor
       unconditionally increments a counter on it before the point where it
       later faulted. Recovering via longjmp never undid that increment,
       and something unrelated (TPlanet.RelationToRanger) went out of sync
       with it later. Temporarily null the context for the exact duration
       of the constructor call so it takes the guarded (no-op) branch
       instead, then restore it immediately -- including on the recovered-
       fault path, so a caught fault doesn't leave the context zeroed for
       the rest of the session. */
    {
        volatile uint32_t context_saved = 0;
        volatile uint32_t original_context = 0;
        uintptr_t context_cell = module_base + CE_RVA_CON_CONTEXT_CELL;

        if (ce_region_has_access((const void *)context_cell, 4u, 0)) {
            original_context = *(const uint32_t *)context_cell;
            context_saved = 1;
        }

        /* Everything from here on calls directly into Delphi-compiled
           engine code from this foreign-compiled frame; see the VEH/setjmp
           comment near the top of the file. Recover to a safe 0 return
           instead of crashing if anything inside faults, and log which
           step was last reached (con-build-progress.log) either way. */
        ce_ensure_veh_installed();
        if (setjmp(g_ce_recovery_point) != 0) {
            if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
                *(uint32_t *)context_cell = original_context;
            }
            ce_write_progress("recovered-from-fault");
            return 0;
        }
        InterlockedExchange(&g_ce_guard_active, 1);

        if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
            *(uint32_t *)context_cell = 0;
        }
        ce_write_progress("before-construct");
        new_con = ce_call_delphi_constructor(con_class_ref, module_base + CE_RVA_TCON_CONSTRUCTOR);
        ce_write_progress("after-construct");
        if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
            *(uint32_t *)context_cell = original_context;
        }
    }
    if (new_con == 0 || !ce_region_has_access((const void *)(uintptr_t)new_con, 0x20u, 1)) {
        InterlockedExchange(&g_ce_guard_active, 0);
        return 0;
    }

    ref_x = 0.0f;
    ref_y = 0.0f;
    if (ce_region_has_access((const void *)(uintptr_t)(old_galaxy_ptr + 0x2cu), 4u, 0)) {
        ref_list = *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + 0x2cu);
        if (ce_region_has_access((const void *)(uintptr_t)ref_list, 12u, 0)) {
            ref_count = *(const uint32_t *)(uintptr_t)(ref_list + 8u);
            if (ref_count > 0u) {
                ref_array = *(const uint32_t *)(uintptr_t)(ref_list + 4u);
                if (ce_region_has_access((const void *)(uintptr_t)ref_array, 4u, 0)) {
                    ref_con = *(const uint32_t *)(uintptr_t)ref_array;
                    if (ce_region_has_access((const void *)(uintptr_t)ref_con, 0x1cu, 0)) {
                        memcpy(&ref_x, (const void *)(uintptr_t)(ref_con + 0x14u), sizeof(ref_x));
                        memcpy(&ref_y, (const void *)(uintptr_t)(ref_con + 0x18u), sizeof(ref_y));
                    }
                }
            }
        }
    }
    ref_x += 500.0f;
    memcpy((void *)(uintptr_t)(new_con + 0x14u), &ref_x, sizeof(ref_x));
    memcpy((void *)(uintptr_t)(new_con + 0x18u), &ref_y, sizeof(ref_y));
    ce_write_progress("after-position-copy");

    ce_call_delphi_method_dword(con_list, new_con, module_base + CE_RVA_LIST_ADD);
    ce_write_progress("after-add");
    InterlockedExchange(&g_ce_guard_active, 0);
    return new_con;
}

/* Empirical probe for the still-unexplained TGalaxy.NextDay crash mentioned
   on CEAdapterMarkSecondGalaxyEntryDisabled: rather than let the engine's
   own turn loop call NextDay on whatever the Galaxy slot points to (where a
   fault is an unguarded, unrecoverable process crash), call it ourselves
   under the VEH/setjmp guard so a fault is caught and reported instead of
   taking the game down. ce_write_fault_report logs the exact faulting EIP
   and (for access violations) the read/write address, which is enough to
   identify which field NextDay dereferences that GenerateStars alone never
   populated. Never touches the live Galaxy slot or g_ce_active_arm -- the
   engine keeps running the real galaxy throughout this call. */
uint32_t CE_CALL CEAdapterProbeNextDayOnSecondGalaxy(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t second_galaxy;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)second_galaxy, 0x1dcu, 0)) {
        return 0;
    }

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_fault_report("nextday-probe-FAULTED");
        InterlockedExchange(&g_ce_guard_active, 0);
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);
    ce_write_progress("nextday-probe:before");
    ce_call_delphi_method_byte(second_galaxy, 0u, module_base + CE_RVA_TGALAXY_NEXTDAY);
    ce_write_progress("nextday-probe:after-ok");
    InterlockedExchange(&g_ce_guard_active, 0);
    return 1;
}

/* GenerateStars alone leaves a TGalaxy missing everything the engine's own
   post-generation bootstrap fills in (name assignment, sectors, economy),
   so an entered-but-empty second galaxy will never become non-empty on a
   later day. Without this, status stays at 2 forever and CE_MapSmoke's Turn
   code re-enters/re-returns the same broken instance every single day-skip
   for the rest of the session -- repeatedly flipping the live engine's
   Galaxy slot in place, which is the likely cause of the save failure seen
   after this happened once. Mark the attempt terminal instead of retrying. */
uint32_t CE_CALL CEAdapterAbandonEmptySecondGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t old_galaxy;
    uint32_t second_galaxy;

    if (InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 || old_galaxy == 0 || galaxy_ptr != second_galaxy ||
        !ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    *galaxy_slot = old_galaxy;
    InterlockedExchange(&g_ce_active_arm, 0);
    InterlockedExchange(&g_ce_second_galaxy_ptr, 0);
    InterlockedExchange(&g_ce_second_generation_status, 4);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

/* One-shot window-tree dump, purely for locating the star-map screen's
   window/control layout from inside the process (no cross-process handle
   permissions needed, unlike external inspection tools). Logs every
   top-level window owned by this process and its full child-window tree
   to process-windows.log so a map-screen button can be positioned and
   parented correctly.

   CRASH LESSON (froze the whole game hard enough that even the OS
   couldn't close it, forcing a taskkill and leaving the desktop briefly
   broken): plain GetWindowTextA sends a blocking WM_GETTEXT to windows not
   owned by the calling thread. If Rangers.exe has any window living on a
   thread other than the one running our Turn-script call (a loading/audio
   worker, for instance) and that thread is waiting on the main thread for
   anything, this deadlocks both threads permanently -- classic Win32 GUI
   deadlock, unrecoverable without killing the process. Use
   SendMessageTimeoutA with SMTO_ABORTIFHUNG instead, which bounds the wait
   and returns instead of hanging forever. GetClassNameA/GetWindowRect never
   send messages (class info and geometry live in kernel window-manager
   state), so they were never the risk. */
static void ce_get_window_title_safe(HWND hwnd, char *out, size_t out_size) {
    DWORD_PTR result = 0;
    LRESULT sent;
    out[0] = 0;
    sent = SendMessageTimeoutA(hwnd, WM_GETTEXT, (WPARAM)out_size, (LPARAM)out,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result);
    if (sent == 0) {
        out[0] = 0;
    } else {
        out[out_size - 1] = 0;
    }
}

static BOOL CALLBACK ce_enum_child_proc(HWND hwnd, LPARAM lparam) {
    char class_name[128];
    char title[256];
    RECT rect;
    char payload[640];
    int size;
    HWND parent;

    (void)lparam;
    class_name[0] = 0;
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    ce_get_window_title_safe(hwnd, title, sizeof(title));
    parent = GetParent(hwnd);
    if (!GetWindowRect(hwnd, &rect)) {
        rect.left = rect.top = rect.right = rect.bottom = 0;
    }
    size = snprintf(payload, sizeof(payload),
        "  CHILD hwnd=0x%08lx parent=0x%08lx id=%d class=\"%s\" title=\"%s\" visible=%d rect=(%ld,%ld,%ld,%ld)\r\n",
        (unsigned long)(uintptr_t)hwnd, (unsigned long)(uintptr_t)parent,
        GetDlgCtrlID(hwnd), class_name, title, IsWindowVisible(hwnd) ? 1 : 0,
        (long)rect.left, (long)rect.top, (long)rect.right, (long)rect.bottom);
    if (size > 0) {
        ce_write_text_marker("process-windows.log", payload, (size_t)size);
    }
    return TRUE;
}

static BOOL CALLBACK ce_enum_top_proc(HWND hwnd, LPARAM lparam) {
    DWORD pid = 0;
    char class_name[128];
    char title[256];
    RECT rect;
    char payload[640];
    int size;

    (void)lparam;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    class_name[0] = 0;
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    ce_get_window_title_safe(hwnd, title, sizeof(title));
    if (!GetWindowRect(hwnd, &rect)) {
        rect.left = rect.top = rect.right = rect.bottom = 0;
    }
    size = snprintf(payload, sizeof(payload),
        "TOP hwnd=0x%08lx class=\"%s\" title=\"%s\" visible=%d rect=(%ld,%ld,%ld,%ld)\r\n",
        (unsigned long)(uintptr_t)hwnd, class_name, title, IsWindowVisible(hwnd) ? 1 : 0,
        (long)rect.left, (long)rect.top, (long)rect.right, (long)rect.bottom);
    if (size > 0) {
        ce_write_text_marker("process-windows.log", payload, (size_t)size);
    }
    EnumChildWindows(hwnd, ce_enum_child_proc, 0);
    return TRUE;
}

uint32_t CE_CALL CEAdapterDumpProcessWindows(void) {
    char header[64];
    int size = snprintf(header, sizeof(header), "--- window dump ---\r\n");
    if (size > 0) {
        ce_write_text_marker("process-windows.log", header, (size_t)size);
    }
    EnumWindows(ce_enum_top_proc, 0);
    return 1;
}

/* Unambiguous, non-visual proof of whether the swap actually changes what
   the rest of the engine considers "the current galaxy": logs the raw
   pointer value the script's own GalaxyPtr() resolves to, every turn, so a
   before/after comparison across a single click doesn't depend on reading
   star names off a screenshot. */
uint32_t CE_CALL CEAdapterProbeRawGalaxyPointer(uint32_t galaxy_ptr, uint32_t turn) {
    char payload[96];
    int size = snprintf(payload, sizeof(payload),
        "{\"turn\":%lu,\"galaxy_ptr\":%lu,\"active_arm\":%ld}\r\n",
        (unsigned long)turn, (unsigned long)galaxy_ptr,
        (long)InterlockedCompareExchange(&g_ce_active_arm, 0, 0));
    if (size > 0) {
        ce_write_text_marker("raw-galaxy-pointer.jsonl", payload, (size_t)size);
    }
    return 1;
}

static uint32_t ce_mix32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

/* A live arm switch must keep every engine reference valid.  Replacing the
   global TGalaxy pointer was proven unsafe (NextDay retained references to
   the old object), so ABI 12 snapshots and edits the loaded TCon/sector
   objects in place.  Object identity, planets, fleets, ownership and route
   targets remain untouched; only the map-facing coordinates and names are
   swapped, and the original values can be restored exactly. */
typedef struct ce_live_con_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
    float old_x;
    float old_y;
} ce_live_con_snapshot;

typedef struct ce_live_sector_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
} ce_live_sector_snapshot;

static ce_live_con_snapshot *g_ce_live_cons = NULL;
static ce_live_sector_snapshot *g_ce_live_sectors = NULL;
static uint32_t g_ce_live_con_count = 0;
static uint32_t g_ce_live_sector_count = 0;
static uint32_t g_ce_live_snapshot_galaxy = 0;

static const wchar_t *const g_ce_second_system_names[] = {
    L"Эльтанская Рана", L"Первый Приют", L"Ковчег-IV", L"Крепость Карх",
    L"Узел Без Лица", L"Люмен", L"Призма Единства", L"Пепельный рынок",
    L"Сиротское гнездо", L"Город Незажжённых", L"Врата Второго Дома",
    L"Латунное Солнце", L"Наковальня Карха", L"Корона Тарга",
    L"Железная Клятва", L"Красная Кузня", L"Покой Молота",
    L"Бронзовый Дозор", L"Стальное Сердце", L"Очаг Воителей",
    L"Девять Щитов", L"Маяк Плавильщиков", L"Базальтовый Трон",
    L"Бледная Маска", L"Скрытый Глаз", L"Безымянный Путь",
    L"Хранилище Шёпота", L"Зеркальная Тень", L"Тайная Нить",
    L"Пустой Свидетель", L"Седьмая Тишина", L"Обсидиановый След",
    L"Закрытая Дверь", L"Немой Оракул", L"Ночной Шифр", L"Медная Чаша",
    L"Общая Мера", L"Купеческий Рассвет", L"Три Договора",
    L"Янтарная Биржа", L"Караванный Очаг", L"Справедливая Цена",
    L"Монета Странника", L"Открытая Книга", L"Позолоченный Тракт",
    L"Тихая Гавань", L"Точка Равновесия", L"Кристальная Теорема",
    L"Живое Уравнение", L"Белый Архив", L"Семь Спектров", L"Свет Разума",
    L"Ось Гармонии", L"Синяя Аксиома", L"Линза Памяти", L"Ясная Мысль",
    L"Хоровое Число", L"Совершенная Орбита", L"Дальняя Гипотеза",
    L"Угольная Дорога", L"Вольный Фонарь", L"Пыльное Убежище",
    L"Последний Караван", L"Серый Колодец", L"Сломанный Компас",
    L"Огонь Скитальцев", L"Крайняя Таверна", L"Ржавый Якорь",
    L"Долгие Сумерки", L"Хлеб Изгнанников", L"Пограничный Колокол",
    L"Споровая Луна", L"Зелёное Эхо", L"Голодная Туманность",
    L"Мёртвый Сад", L"Хитиновый Разлом", L"Заражённый Маяк",
    L"Безмолвный Рой", L"Костяная Орбита", L"Кислотный Рассвет",
    L"Пустой Кокон", L"Прилив Шрама", L"Последнее Противоядие"
};

static const wchar_t *const g_ce_second_sector_names[] = {
    L"Кузни Карха", L"Железный Предел", L"Бастион Тарг", L"Клин Молота",
    L"Безликая Глубина", L"Тихий Излом", L"Чёрный Узел",
    L"Завеса Неведомых", L"Люменский Пояс", L"Террасы Меры",
    L"Медный Реестр", L"Долина Договоров", L"Призматический Хор",
    L"Решётка Эха", L"Сияющий Архив", L"Контур Единства",
    L"Пепельная окраина", L"Вольные Пустоши", L"Караванный Разлом",
    L"Серые Причалы", L"Клисанский шрам", L"Зелёная Рана",
    L"Споровый Рубеж", L"Немая Зараза"
};

/* This 32-bit Delphi build stores the UTF-16 string byte length (BSTR-style),
   not the character count, in the dword immediately before its data.  The
   preceding dword is kept immortal so the engine can retain the pointer. */
static uint32_t ce_make_immortal_unicode(const wchar_t *text) {
    size_t length = wcslen(text);
    size_t bytes = 8u + (length + 1u) * sizeof(wchar_t);
    unsigned char *block = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (block == NULL || length > 0x7fffffffu) return 0;
    *(int32_t *)(void *)block = -1;
    *(uint32_t *)(void *)(block + 4u) = (uint32_t)(length * sizeof(wchar_t));
    memcpy(block + 8u, text, (length + 1u) * sizeof(wchar_t));
    return (uint32_t)(uintptr_t)(block + 8u);
}

static int ce_read_plain_list(uint32_t list, uint32_t *array_out, uint32_t *count_out) {
    uint32_t array;
    uint32_t count;
    if (!ce_region_has_access((const void *)(uintptr_t)list, 12u, 0)) return 0;
    array = *(const uint32_t *)(uintptr_t)(list + 4u);
    count = *(const uint32_t *)(uintptr_t)(list + 8u);
    if (count == 0u || count > 10000u ||
        !ce_region_has_access((const void *)(uintptr_t)array, count * 4u, 0)) return 0;
    *array_out = array;
    *count_out = count;
    return 1;
}

static int ce_live_read_wide_string(uint32_t pointer, const wchar_t **text_out,
        uint32_t *characters_out) {
    uint32_t byte_length;
    const wchar_t *text;
    uint32_t characters, index;
    int has_cyrillic = 0;
    if (pointer < 0x10000u ||
            !ce_region_has_access((const void *)(uintptr_t)(pointer - 4u), 4u, 0)) return 0;
    byte_length = *(const uint32_t *)(uintptr_t)(pointer - 4u);
    if (byte_length < 2u || byte_length > 96u || (byte_length & 1u) != 0u ||
            !ce_region_has_access((const void *)(uintptr_t)pointer, byte_length, 0)) return 0;
    characters = byte_length / 2u;
    text = (const wchar_t *)(uintptr_t)pointer;
    for (index = 0; index < characters; ++index) {
        wchar_t ch = text[index];
        if (ch >= 0x0400 && ch <= 0x052fu) has_cyrillic = 1;
        else if (ch != L' ' && ch != L'-' && ch != L'\'' &&
                !(ch >= L'0' && ch <= L'9')) return 0;
    }
    if (!has_cyrillic) return 0;
    *text_out = text;
    *characters_out = characters;
    return 1;
}

static int ce_live_row_index_matches(uint32_t pointer, uint32_t expected) {
    const wchar_t *text;
    uint32_t byte_length, characters;
    wchar_t first, second;
    if (pointer < 0x10000u ||
            !ce_region_has_access((const void *)(uintptr_t)(pointer - 4u), 4u, 0)) return 0;
    byte_length = *(const uint32_t *)(uintptr_t)(pointer - 4u);
    characters = expected < 10u ? 1u : 2u;
    if (byte_length != characters * 2u ||
            !ce_region_has_access((const void *)(uintptr_t)pointer, byte_length, 0)) return 0;
    text = (const wchar_t *)(uintptr_t)pointer;
    first = (wchar_t)(L'0' + (expected < 10u ? expected : expected / 10u));
    if (text[0] != first) return 0;
    if (characters == 2u) {
        second = (wchar_t)(L'0' + (expected % 10u));
        if (text[1] != second) return 0;
    }
    return 1;
}

/* The map owns a linked TStringList of currently opened sectors.  Each row
   has its previous row at +4, decimal key at +0x14 and generated name at
   +0x18.  Closed sectors are deliberately absent (vanilla adds them when
   bought), so the count is discovered rather than fixed at nineteen. */
static int ce_find_live_sector_rows(uint32_t rows[24], uint32_t *count_out) {
    SYSTEM_INFO system_info;
    uintptr_t address, maximum;
    uint32_t best_rows[24];
    uint32_t best_count = 0u;
    uint32_t row_vmt = (uint32_t)(uintptr_t)GetModuleHandleW(NULL) +
        (0x008273a8u - 0x00400000u);
    GetSystemInfo(&system_info);
    address = (uintptr_t)system_info.lpMinimumApplicationAddress;
    maximum = (uintptr_t)system_info.lpMaximumApplicationAddress;
    if (maximum > UINT32_MAX) maximum = UINT32_MAX;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory;
        uintptr_t start, end, cursor, next;
        DWORD page;
        if (VirtualQuery((const void *)address, &memory, sizeof(memory)) != sizeof(memory)) {
            address += 0x10000u;
            continue;
        }
        start = (uintptr_t)memory.BaseAddress;
        next = start + memory.RegionSize;
        if (next <= address) break;
        page = memory.Protect & 0xffu;
        if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE &&
                (memory.Protect & PAGE_GUARD) == 0u &&
                (page == PAGE_READWRITE || page == PAGE_WRITECOPY ||
                 page == PAGE_EXECUTE_READWRITE || page == PAGE_EXECUTE_WRITECOPY) &&
                memory.RegionSize >= 0x30u) {
            end = next - 0x30u;
            cursor = (start + 3u) & ~(uintptr_t)3u;
            for (; cursor <= end; cursor += 4u) {
                uint32_t tail;
                if (*(const uint32_t *)cursor != row_vmt) continue;
                for (tail = 4u; tail < 24u; ++tail) {
                    uint32_t current = (uint32_t)cursor;
                    uint32_t remaining = tail + 1u;
                    while (remaining != 0u) {
                        uint32_t index = remaining - 1u;
                        uint32_t key, name, characters;
                        const wchar_t *unused_text;
                        if (!ce_region_has_access((const void *)(uintptr_t)current, 0x1cu, 0) ||
                                *(const uint32_t *)(uintptr_t)current != row_vmt) break;
                        key = *(const uint32_t *)(uintptr_t)(current + 0x14u);
                        name = *(const uint32_t *)(uintptr_t)(current + 0x18u);
                        if (!ce_live_row_index_matches(key, index) ||
                                !ce_live_read_wide_string(name, &unused_text, &characters)) break;
                        rows[index] = current;
                        if (index != 0u)
                            current = *(const uint32_t *)(uintptr_t)(current + 4u);
                        --remaining;
                    }
                    if (remaining == 0u && tail + 1u > best_count) {
                        best_count = tail + 1u;
                        memcpy(best_rows, rows, best_count * sizeof(best_rows[0]));
                    }
                }
            }
        }
        address = next;
    }
    if (best_count == 0u) return 0;
    memcpy(rows, best_rows, best_count * sizeof(rows[0]));
    *count_out = best_count;
    return 1;
}

static int ce_capture_live_arm(uint32_t galaxy_ptr) {
    uint32_t con_list, con_array, con_count;
    uint32_t sector_rows[24];
    uint32_t sector_count = 0u;
    uint32_t index;
    if (g_ce_live_snapshot_galaxy == galaxy_ptr && g_ce_live_cons != NULL) return 1;
    char diagnostic[256];
    int diagnostic_size;
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x38u), 4u, 0)) {
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"capture-failed\",\"stage\":\"galaxy\"}\r\n", 47u);
        return 0;
    }
    con_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    if (!ce_read_plain_list(con_list, &con_array, &con_count)) {
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"capture-failed\",\"stage\":\"systems\"}\r\n", 48u);
        return 0;
    }
    if (!ce_find_live_sector_rows(sector_rows, &sector_count)) {
        diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
            "{\"status\":\"capture-failed\",\"stage\":\"sector-rows\"}\r\n");
        if (diagnostic_size > 0) ce_write_text_marker(
            "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
        return 0;
    }
    g_ce_live_cons = (ce_live_con_snapshot *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, con_count * sizeof(*g_ce_live_cons));
    g_ce_live_sectors = (ce_live_sector_snapshot *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sector_count * sizeof(*g_ce_live_sectors));
    if (g_ce_live_cons == NULL || g_ce_live_sectors == NULL) return 0;
    for (index = 0; index < con_count; ++index) {
        uint32_t object = *(const uint32_t *)(uintptr_t)(con_array + index * 4u);
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x1cu, 1)) return 0;
        g_ce_live_cons[index].object = object;
        g_ce_live_cons[index].old_name = *(const uint32_t *)(uintptr_t)(object + 0x10u);
        memcpy(&g_ce_live_cons[index].old_x,
            (const void *)(uintptr_t)(object + 0x14u), sizeof(float));
        memcpy(&g_ce_live_cons[index].old_y,
            (const void *)(uintptr_t)(object + 0x18u), sizeof(float));
        g_ce_live_cons[index].second_name = ce_make_immortal_unicode(
            g_ce_second_system_names[index % (sizeof(g_ce_second_system_names) /
                sizeof(g_ce_second_system_names[0]))]);
        if (g_ce_live_cons[index].second_name == 0u) return 0;
    }
    for (index = 0; index < sector_count; ++index) {
        uint32_t object = sector_rows[index];
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x1cu, 1)) return 0;
        g_ce_live_sectors[index].object = object;
        g_ce_live_sectors[index].old_name = *(const uint32_t *)(uintptr_t)(object + 0x18u);
        g_ce_live_sectors[index].second_name = ce_make_immortal_unicode(
            g_ce_second_sector_names[index % (sizeof(g_ce_second_sector_names) /
                sizeof(g_ce_second_sector_names[0]))]);
        if (g_ce_live_sectors[index].second_name == 0u) return 0;
    }
    g_ce_live_con_count = con_count;
    g_ce_live_sector_count = sector_count;
    g_ce_live_snapshot_galaxy = galaxy_ptr;
    diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"captured\",\"systems\":%u,\"sectors\":%u}\r\n",
        con_count, sector_count);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    return 1;
}

static void ce_apply_live_arm(int second_home) {
    uint32_t index;
    uint32_t seed = (uint32_t)InterlockedCompareExchange(&g_ce_live_seed, 0, 0);
    float min_x = 10000000.0f, max_x = -10000000.0f;
    float min_y = 10000000.0f, max_y = -10000000.0f;
    char report[256];
    int report_size;
    if (g_ce_live_cons == NULL || g_ce_live_sectors == NULL) return;
    for (index = 0; index < g_ce_live_con_count; ++index) {
        float x = g_ce_live_cons[index].old_x;
        float y = g_ce_live_cons[index].old_y;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    for (index = 0; index < g_ce_live_con_count; ++index) {
        ce_live_con_snapshot *item = &g_ce_live_cons[index];
        float x = item->old_x;
        float y = item->old_y;
        if (second_home) {
            uint32_t id = *(const uint32_t *)(uintptr_t)(item->object + 4u);
            uint32_t hx = ce_mix32(seed ^ id ^ (index * 0x9e3779b9u));
            uint32_t hy = ce_mix32((seed + 0x85ebca6bu) ^ id ^ (index * 0xc2b2ae35u));
            x = min_x + ((float)(hx & 0xffffu) / 65535.0f) * (max_x - min_x);
            y = min_y + ((float)(hy & 0xffffu) / 65535.0f) * (max_y - min_y);
        }
        *(uint32_t *)(uintptr_t)(item->object + 0x10u) =
            second_home ? item->second_name : item->old_name;
        memcpy((void *)(uintptr_t)(item->object + 0x14u), &x, sizeof(x));
        memcpy((void *)(uintptr_t)(item->object + 0x18u), &y, sizeof(y));
    }
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sectors[index];
        *(uint32_t *)(uintptr_t)(item->object + 0x18u) =
            second_home ? item->second_name : item->old_name;
    }
    InterlockedExchange(&g_ce_active_arm, second_home ? 1 : 0);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"switched\",\"abi\":%u,\"arm\":\"%s\","
        "\"systems\":%u,\"sectors\":%u}\r\n",
        CEAdapterAbiVersion(), second_home ? "SECOND_HOME" : "OLD_ARM",
        g_ce_live_con_count, g_ce_live_sector_count);
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
}

static BOOL CALLBACK ce_invalidate_process_window(HWND hwnd, LPARAM unused) {
    DWORD pid = 0;
    (void)unused;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) InvalidateRect(hwnd, NULL, TRUE);
    return TRUE;
}

static LRESULT CALLBACK ce_loading_window_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam
) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT rect;
        HDC dc = BeginPaint(hwnd, &paint);
        HBRUSH background = CreateSolidBrush(RGB(2, 10, 28));
        HFONT font = CreateFontW(-42, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
        HFONT old_font;
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(110, 220, 255));
        old_font = (HFONT)SelectObject(dc, font);
        DrawTextW(dc, L"РЕЗОНАНСНЫЙ ПЕРЕХОД", -1, &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        rect.top += 110;
        SetTextColor(dc, RGB(210, 240, 255));
        SelectObject(dc, old_font);
        DeleteObject(font);
        font = CreateFontW(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
        old_font = (HFONT)SelectObject(dc, font);
        DrawTextW(dc, InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0
            ? L"Загрузка Второго Дома..." : L"Возвращение в Первый рукав...",
            -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteObject(background);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static HWND ce_show_loading_window(void) {
    WNDCLASSW window_class;
    HWND owner = NULL;
    HWND window;
    RECT rect;
    if (g_ce_loading_window_class == 0) {
        memset(&window_class, 0, sizeof(window_class));
        window_class.lpfnWndProc = ce_loading_window_proc;
        window_class.hInstance = (HINSTANCE)GetModuleHandleW(NULL);
        window_class.hCursor = LoadCursor(NULL, IDC_WAIT);
        window_class.lpszClassName = L"ChildrenOfEltanArmLoading";
        g_ce_loading_window_class = RegisterClassW(&window_class);
        if (g_ce_loading_window_class == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return NULL;
        }
    }
    {
        HWND candidate = GetTopWindow(NULL);
        while (candidate != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(candidate, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(candidate)) {
                owner = candidate;
                break;
            }
            candidate = GetNextWindow(candidate, GW_HWNDNEXT);
        }
    }
    if (owner != NULL && GetWindowRect(owner, &rect)) {
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"ChildrenOfEltanArmLoading", L"Children of Eltan", WS_POPUP,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            owner, NULL, (HINSTANCE)GetModuleHandleW(NULL), NULL);
    } else {
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"ChildrenOfEltanArmLoading", L"Children of Eltan", WS_POPUP,
            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            NULL, NULL, (HINSTANCE)GetModuleHandleW(NULL), NULL);
    }
    if (window != NULL) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    return window;
}

static LRESULT CALLBACK ce_live_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && wparam == VK_F8 && (lparam & (1L << 31)) == 0 &&
        (lparam & (1L << 30)) == 0 &&
        InterlockedCompareExchange(&g_ce_live_switch_lock, 1, 0) == 0) {
        uint32_t galaxy_ptr = (uint32_t)InterlockedCompareExchange(
            &g_ce_live_galaxy_ptr, 0, 0);
        if (ce_capture_live_arm(galaxy_ptr)) {
            HWND game_window = GetForegroundWindow();
            DWORD game_pid = 0;
            GetWindowThreadProcessId(game_window, &game_pid);
            if (game_pid != GetCurrentProcessId()) game_window = NULL;
            HWND loading = ce_show_loading_window();
            Sleep(700u);
            ce_apply_live_arm(InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0);
            Sleep(450u);
            if (loading != NULL) DestroyWindow(loading);
            EnumWindows(ce_invalidate_process_window, 0);
            /* The galaxy map snapshots labels when it opens.  A portal entered
               from the map therefore returns the player to local space; opening
               the map again builds the destination view from the new arm. */
            if (game_window != NULL) {
                ShowWindow(game_window, SW_RESTORE);
                SetForegroundWindow(game_window);
                PostMessageW(game_window, WM_KEYDOWN, (WPARAM)'M', 0x00320001L);
                PostMessageW(game_window, WM_KEYUP, (WPARAM)'M', 0xc0320001L);
            }
        }
        InterlockedExchange(&g_ce_live_switch_lock, 0);
        return 1;
    }
    return CallNextHookEx(g_ce_keyboard_hook, code, wparam, lparam);
}

uint32_t CE_CALL CEAdapterInstallLiveArmSwitch(uint32_t galaxy_ptr, uint32_t seed) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    InterlockedExchange(&g_ce_live_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_live_seed, (LONG)seed);
    if (!ce_capture_live_arm(galaxy_ptr)) return 0;
    if (g_ce_keyboard_hook == NULL) {
        g_ce_keyboard_hook = SetWindowsHookExA(
            WH_KEYBOARD, ce_live_keyboard_proc, NULL, GetCurrentThreadId());
        if (g_ce_keyboard_hook == NULL) return 0;
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"ready\",\"hotkey\":\"F8\"}\r\n", 34u);
    }
    return 1;
}

__attribute__((used)) static void CE_CALL ce_transform_loaded_second_home(
    uint32_t galaxy_ptr, uint32_t reader
) {
    uint32_t list;
    uint32_t array;
    uint32_t count;
    uint32_t index;
    uint32_t transformed = 0;
    uint32_t seed;
    float min_x = 10000000.0f;
    float max_x = -10000000.0f;
    float min_y = 10000000.0f;
    float max_y = -10000000.0f;
    char report[256];
    int report_size;

    (void)reader;
    if (InterlockedExchange(&g_ce_load_transform_armed, 0) == 0) return;
    seed = (uint32_t)InterlockedCompareExchange(&g_ce_load_transform_seed, 0, 0);
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x2cu), 4u, 0)) {
        ce_write_text_marker("second-home-transform.jsonl",
            "{\"status\":\"galaxy-unreadable\"}\r\n", 32u);
        return;
    }
    list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    if (!ce_region_has_access((const void *)(uintptr_t)list, 12u, 0)) return;
    array = *(const uint32_t *)(uintptr_t)(list + 4u);
    count = *(const uint32_t *)(uintptr_t)(list + 8u);
    if (count < 2u || count > 10000u ||
        !ce_region_has_access((const void *)(uintptr_t)array, count * 4u, 0)) return;

    for (index = 0; index < count; ++index) {
        uint32_t con = *(const uint32_t *)(uintptr_t)(array + index * 4u);
        float x;
        float y;
        if (!ce_region_has_access((const void *)(uintptr_t)con, 0x1cu, 0)) continue;
        memcpy(&x, (const void *)(uintptr_t)(con + 0x14u), sizeof(x));
        memcpy(&y, (const void *)(uintptr_t)(con + 0x18u), sizeof(y));
        if (x < -10000000.0f || x > 10000000.0f ||
            y < -10000000.0f || y > 10000000.0f) continue;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    if (min_x >= max_x || min_y >= max_y) return;

    for (index = 0; index < count; ++index) {
        uint32_t con = *(const uint32_t *)(uintptr_t)(array + index * 4u);
        uint32_t id;
        uint32_t hx;
        uint32_t hy;
        float x;
        float y;
        if (!ce_region_has_access((const void *)(uintptr_t)con, 0x1cu, 1)) continue;
        id = *(const uint32_t *)(uintptr_t)(con + 0x04u);
        hx = ce_mix32(seed ^ id ^ (index * 0x9e3779b9u));
        hy = ce_mix32((seed + 0x85ebca6bu) ^ id ^ (index * 0xc2b2ae35u));
        x = min_x + ((float)(hx & 0xffffu) / 65535.0f) * (max_x - min_x);
        y = min_y + ((float)(hy & 0xffffu) / 65535.0f) * (max_y - min_y);
        memcpy((void *)(uintptr_t)(con + 0x14u), &x, sizeof(x));
        memcpy((void *)(uintptr_t)(con + 0x18u), &y, sizeof(y));
        ++transformed;
    }
    InterlockedExchange(&g_ce_active_arm, 1);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"transformed\",\"abi\":%u,\"seed\":%u,"
        "\"count\":%u,\"bounds\":[%.2f,%.2f,%.2f,%.2f]}\r\n",
        CEAdapterAbiVersion(), seed, transformed, min_x, max_x, min_y, max_y);
    if (report_size > 0) ce_write_text_marker(
        "second-home-transform.jsonl", report, (size_t)report_size
    );
}

#if defined(__i386__)
__attribute__((naked)) static void ce_tgalaxy_load_hook(void) {
    __asm__ volatile(
        "pushl %edx\n\t"
        "pushl %eax\n\t"
        "call *_g_ce_load_trampoline\n\t"
        "popl %ecx\n\t"
        "popl %edx\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"
        "call _ce_transform_loaded_second_home\n\t"
        "addl $8, %esp\n\t"
        "ret\n\t"
    );
}
#endif

uint32_t CE_CALL CEAdapterPollSecondHomeTransform(
    uint32_t galaxy_ptr, uint32_t seed
) {
#if defined(__i386__)
    static const unsigned char load_signature[] = {
        0x55, 0x8b, 0xec, 0xb9, 0x44, 0x00, 0x00, 0x00,
        0x6a, 0x00, 0x6a, 0x00, 0x49, 0x75, 0xf9, 0x51
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[8];
    unsigned char *trampoline;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;
    int32_t relative;

    /* Existing saves retain their compiled Turn graph.  ABI 11 saves call
       this export already, so use it as the compatibility bridge that
       installs the ABI 12 portal hotkey without requiring a new campaign. */
    CEAdapterInstallLiveArmSwitch(galaxy_ptr, seed);

    if (GetFileAttributesA("C:\\ce_debug\\arm-second-home.flag") == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    target = (unsigned char *)(module_base + CE_RVA_TGALAXY_LOAD_FROM_STREAM);
    hook_state = InterlockedCompareExchange(&g_ce_load_hook_installed, -1, 0);
    if (hook_state == 0) {
        if (memcmp(target, load_signature, sizeof(load_signature)) != 0) {
            InterlockedExchange(&g_ce_load_hook_installed, 0);
            return 0;
        }
        trampoline = (unsigned char *)VirtualAlloc(
            NULL, 13u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        if (trampoline == NULL) {
            InterlockedExchange(&g_ce_load_hook_installed, 0);
            return 0;
        }
        memcpy(trampoline, target, 8u);
        trampoline[8] = 0xe9;
        relative = (int32_t)((target + 8u) - (trampoline + 13u));
        memcpy(trampoline + 9u, &relative, sizeof(relative));
        g_ce_load_trampoline = trampoline;
        FlushInstructionCache(GetCurrentProcess(), trampoline, 13u);
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xe9;
        relative = (int32_t)((unsigned char *)(uintptr_t)ce_tgalaxy_load_hook -
            (target + 5u));
        memcpy(patch + 1u, &relative, sizeof(relative));
        if (!VirtualProtect(target, 8u, PAGE_EXECUTE_READWRITE, &old_protect)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            g_ce_load_trampoline = NULL;
            InterlockedExchange(&g_ce_load_hook_installed, 0);
            return 0;
        }
        memcpy(target, patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
        VirtualProtect(target, 8u, old_protect, &ignored_protect);
        InterlockedExchange(&g_ce_load_hook_installed, 1);
    } else if (hook_state != 1) {
        return 0;
    }
    InterlockedExchange(&g_ce_load_transform_seed, (LONG)seed);
    InterlockedExchange(&g_ce_load_transform_armed, 1);
    DeleteFileA("C:\\ce_debug\\arm-second-home.flag");
    ce_write_text_marker("second-home-transform.jsonl",
        "{\"status\":\"armed\"}\r\n", 20u);
    return 1;
#else
    (void)galaxy_ptr;
    (void)seed;
    return 0;
#endif
}

/* SaveToStream cannot safely be called from a Turn callback: the real game
   test deadlocked before producing a byte.  The safe place is the engine's
   own save operation.  This detour lets the original serializer run first,
   then copies its completed buffer without re-entering any Delphi code.

   Six bytes are displaced because the first three x86 instructions are
   1+2+3 bytes.  The trampoline executes those complete instructions and
   jumps back to SaveToStream+6. */
__attribute__((used)) static void CE_CALL ce_capture_completed_galaxy_save(
    uint32_t galaxy_ptr, uint32_t buffer
) {
    uint32_t expected;
    uint32_t length;
    uint32_t capacity;
    uint32_t position;
    uint32_t data;
    uint32_t hash;
    char report[224];
    int report_size;
    int wrote;

    if (InterlockedCompareExchange(&g_ce_save_hook_armed, 0, 0) == 0) return;
    expected = (uint32_t)InterlockedCompareExchange(
        &g_ce_save_hook_expected_galaxy, 0, 0
    );
    if (expected == 0 || galaxy_ptr != expected) return;
    if (InterlockedExchange(&g_ce_save_hook_armed, 0) == 0) return;
    if (!ce_region_has_access((const void *)(uintptr_t)buffer, 0x14u, 0)) {
        ce_write_text_marker("galaxy-save-snapshot.jsonl",
            "{\"status\":\"invalid-buffer\"}\r\n", 29u);
        return;
    }

    length = *(const uint32_t *)(uintptr_t)(buffer + 0x04u);
    capacity = *(const uint32_t *)(uintptr_t)(buffer + 0x08u);
    position = *(const uint32_t *)(uintptr_t)(buffer + 0x0cu);
    data = *(const uint32_t *)(uintptr_t)(buffer + 0x10u);
    if (length == 0 || length > capacity || position != length ||
        length > 256u * 1024u * 1024u ||
        !ce_region_has_access((const void *)(uintptr_t)data, length, 0)) {
        report_size = snprintf(report, sizeof(report),
            "{\"status\":\"invalid-layout\",\"length\":%u,"
            "\"position\":%u,\"capacity\":%u}\r\n",
            length, position, capacity);
        if (report_size > 0) ce_write_text_marker(
            "galaxy-save-snapshot.jsonl", report, (size_t)report_size
        );
        return;
    }

    hash = ce_fnv1a32((const unsigned char *)(uintptr_t)data, length);
    wrote = ce_write_binary_file("C:\\ce_debug", "galaxy-save-snapshot.bin",
        (const void *)(uintptr_t)data, length);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"%s\",\"abi\":%u,\"galaxy_ptr\":%u,"
        "\"length\":%u,\"fnv1a32\":%u}\r\n",
        wrote ? "captured" : "write-failed", CEAdapterAbiVersion(),
        galaxy_ptr, length, hash);
    if (report_size > 0) ce_write_text_marker(
        "galaxy-save-snapshot.jsonl", report, (size_t)report_size
    );
    if (wrote) InterlockedExchange(&g_ce_snapshot_done, 1);
}

#if defined(__i386__)
__attribute__((naked)) static void ce_tgalaxy_save_hook(void) {
    __asm__ volatile(
        "pushl %edx\n\t"
        "pushl %eax\n\t"
        "call *_g_ce_save_trampoline\n\t"
        "popl %ecx\n\t"
        "popl %edx\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"
        "call _ce_capture_completed_galaxy_save\n\t"
        "addl $8, %esp\n\t"
        "ret\n\t"
    );
}
#endif

uint32_t CE_CALL CEAdapterArmGalaxySaveSnapshot(uint32_t galaxy_ptr) {
#if defined(__i386__)
    static const unsigned char save_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xa0, 0x33, 0xc9,
        0x89, 0x4d, 0xa0, 0x89, 0x55, 0xf8, 0x89, 0x45
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[6];
    unsigned char *trampoline;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;
    int32_t relative;

    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    target = (unsigned char *)(module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM);
    hook_state = InterlockedCompareExchange(&g_ce_save_hook_installed, -1, 0);
    if (hook_state == 0) {
        if (memcmp(target, save_signature, sizeof(save_signature)) != 0) {
            InterlockedExchange(&g_ce_save_hook_installed, 0);
            return 0;
        }
        trampoline = (unsigned char *)VirtualAlloc(
            NULL, 11u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        if (trampoline == NULL) {
            InterlockedExchange(&g_ce_save_hook_installed, 0);
            return 0;
        }
        memcpy(trampoline, target, 6u);
        trampoline[6] = 0xe9;
        relative = (int32_t)((target + 6u) - (trampoline + 11u));
        memcpy(trampoline + 7u, &relative, sizeof(relative));
        g_ce_save_trampoline = trampoline;
        FlushInstructionCache(GetCurrentProcess(), trampoline, 11u);

        patch[0] = 0xe9;
        relative = (int32_t)((unsigned char *)(uintptr_t)ce_tgalaxy_save_hook -
            (target + 5u));
        memcpy(patch + 1u, &relative, sizeof(relative));
        patch[5] = 0x90;
        if (!VirtualProtect(target, 6u, PAGE_EXECUTE_READWRITE, &old_protect)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            g_ce_save_trampoline = NULL;
            InterlockedExchange(&g_ce_save_hook_installed, 0);
            return 0;
        }
        memcpy(target, patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
        VirtualProtect(target, 6u, old_protect, &ignored_protect);
        InterlockedExchange(&g_ce_save_hook_installed, 1);
    } else if (hook_state != 1) {
        return 0;
    }

    InterlockedExchange(&g_ce_save_hook_expected_galaxy, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_save_hook_armed, 1);
    ce_write_text_marker("galaxy-save-snapshot.jsonl",
        "{\"status\":\"armed\"}\r\n", 20u);
    return 1;
#else
    (void)galaxy_ptr;
    return 0;
#endif
}

uint32_t CE_CALL CEAdapterSnapshotGalaxy(uint32_t galaxy_ptr) {
    static const unsigned char save_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xa0, 0x33, 0xc9,
        0x89, 0x4d, 0xa0, 0x89, 0x55, 0xf8, 0x89, 0x45
    };
    static const unsigned char buffer_ctor_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xf8, 0x84, 0xd2,
        0x74, 0x08, 0x83, 0xc4, 0xf0, 0xe8
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t buffer_class_ref;
    uint32_t buffer = 0;
    uint32_t length;
    uint32_t capacity;
    uint32_t position;
    uint32_t data;
    uint32_t hash;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char report[192];
    int report_size;
    int wrote_temp;

    if (InterlockedCompareExchange(&g_ce_snapshot_done, 0, 0) != 0) return 2;
    if (InterlockedCompareExchange(&g_ce_snapshot_lock, 1, 0) != 0) return 0;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref) ||
        memcmp((const void *)(module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM),
            save_signature, sizeof(save_signature)) != 0 ||
        memcmp((const void *)(module_base + CE_RVA_BUFFER_CONSTRUCTOR),
            buffer_ctor_signature, sizeof(buffer_ctor_signature)) != 0 ||
        !ce_region_has_access(
            (const void *)(module_base + CE_RVA_BUFFER_CLASS_CELL), 4u, 0)) {
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }

    buffer_class_ref = *(const uint32_t *)(module_base + CE_RVA_BUFFER_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)buffer_class_ref, 4u, 0)) {
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }
    buffer = ce_call_delphi_constructor(
        buffer_class_ref, module_base + CE_RVA_BUFFER_CONSTRUCTOR
    );
    if (buffer == 0 || !ce_region_has_access((const void *)(uintptr_t)buffer, 0x14u, 1)) {
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }

    ce_call_delphi_method_dword(
        galaxy_ptr, buffer, module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM
    );
    length = *(const uint32_t *)(uintptr_t)(buffer + 0x04u);
    capacity = *(const uint32_t *)(uintptr_t)(buffer + 0x08u);
    position = *(const uint32_t *)(uintptr_t)(buffer + 0x0cu);
    data = *(const uint32_t *)(uintptr_t)(buffer + 0x10u);
    if (length == 0 || length > capacity || position != length ||
        length > 256u * 1024u * 1024u ||
        !ce_region_has_access((const void *)(uintptr_t)data, length, 0)) {
        ce_call_delphi_method(buffer, module_base + CE_RVA_TOBJECT_FREE);
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }

    hash = ce_fnv1a32((const unsigned char *)(uintptr_t)data, length);
    ce_write_binary_file("C:\\ce_debug", "galaxy-snapshot.bin",
        (const void *)(uintptr_t)data, length);
    wrote_temp = 0;
    if (GetTempPathA(MAX_PATH, temp_path) != 0 &&
        snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) >= 0) {
        wrote_temp = ce_write_binary_file(marker_dir, "galaxy-snapshot.bin",
            (const void *)(uintptr_t)data, length);
    }
    report_size = snprintf(report, sizeof(report),
        "{\"abi\":%u,\"length\":%u,\"position\":%u,\"capacity\":%u,"
        "\"fnv1a32\":%u,\"temp_written\":%s}\r\n",
        CEAdapterAbiVersion(), length, position, capacity, hash,
        wrote_temp ? "true" : "false");
    if (report_size > 0) {
        ce_write_text_marker("galaxy-snapshot.jsonl", report, (size_t)report_size);
    }
    ce_call_delphi_method(buffer, module_base + CE_RVA_TOBJECT_FREE);
    if (wrote_temp) InterlockedExchange(&g_ce_snapshot_done, 1);
    InterlockedExchange(&g_ce_snapshot_lock, 0);
    return wrote_temp ? 1u : 0u;
}
