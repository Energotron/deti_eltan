#include "ce_second_map_adapter.h"

#include <windows.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
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
static HINSTANCE g_ce_adapter_instance = NULL;

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

/* Defined further down, alongside the WH_KEYBOARD_LL hook it installs;
   forward-declared so DllMain can start it immediately on load instead of
   lazily from inside a Turn-script call (see the comment on that thread
   proc for why: spawning it from Turn-reachable code reproduced a real
   TGalaxy.NextDay crash on turn 0 of a brand new game, and moving the
   spawn here -- which fires once at process attach, before any galaxy or
   Turn processing exists -- was the only variant that stopped it). */
static DWORD WINAPI ce_hook_thread_proc(LPVOID unused);
static DWORD WINAPI ce_sector_spawn_thread_proc(LPVOID unused);

/* Unconditional load marker: proves the engine actually mapped this DLL
   into its process, independent of whether any exported function is ever
   invoked from a Turn script. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        g_ce_adapter_instance = instance;
        DisableThreadLibraryCalls(instance);
        ce_write_progress("dllmain-process-attach");
        thread = CreateThread(NULL, 0, ce_hook_thread_proc, NULL, 0, NULL);
        if (thread != NULL) CloseHandle(thread);
        /* Started here, at process attach, for the exact same reason as
           ce_hook_thread_proc above: anything reachable from Turn-code
           appears to run nested inside TGalaxy.NextDay's own call stack,
           and a heavy OS call made from there -- CreateThread previously,
           now confirmed CreateProcess too (see ce_spawn_sector_name_helper
           and ce_sector_spawn_thread_proc) -- can crash the game. This
           thread sleeps and polls a plain flag instead of ever being
           called into from Turn-code, so the actual CreateProcess call
           never happens on a NextDay-reachable stack. */
        thread = CreateThread(NULL, 0, ce_sector_spawn_thread_proc, NULL, 0, NULL);
        if (thread != NULL) CloseHandle(thread);
        /* ce_find_sector_name_references disabled: spawning it here
           correlated with a fresh game crash on new-game creation.
           Moving it to a background thread avoided the specific
           Turn-code-nested-in-NextDay hazard, but the scan itself (up to
           1GB, byte-unaligned, up to 8 nested full-address-space passes)
           is heavy enough that something else about it -- sustained CPU/
           memory-bandwidth contention during process startup, or a
           genuine bug in the scan itself -- needs to be isolated with a
           clean before/after test before trying again, not patched
           blindly a second time. */
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
static volatile LONG g_ce_portal_status = 0;
static volatile LONG g_ce_portal_hole_id = 0;
static volatile LONG g_ce_portal_galaxy_ptr = 0;
static HHOOK g_ce_keyboard_hook = NULL;
static volatile LONG g_ce_f8_down = 0;
static volatile LONG g_ce_cheat_triggered = 0;
static uint32_t g_ce_cheat_progress = 0u;

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
        CE_CAP_LIVE_ARM_SWITCH |
        CE_CAP_MANUAL_PORTAL_TRANSIT |
        CE_CAP_MAP_VISUAL_FIXES;
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
    uint32_t sector_object;
    /* Old-arm baseline for StarOwner()==1 (Dominators), captured once per
       system_index by CEAdapterCaptureDominatorFlag (called from RScript,
       which is the only side that can call the real StarOwner() built-in --
       this DLL has no way to invoke RScript's own functions, only the
       reverse). dominator_captured guards the idempotent "first value seen
       wins" capture: the per-star Turn-code loop that calls this runs every
       turn, including while Second Home's own suppression is active, so
       without this guard a later (already-suppressed) read would silently
       overwrite the true baseline. */
    uint32_t old_dominator;
    uint32_t dominator_captured;
} ce_live_con_snapshot;

typedef struct ce_live_sector_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
    uint32_t second_name_index;
    uint32_t old_visible;
    uint32_t old_sector_number;
} ce_live_sector_snapshot;

static ce_live_con_snapshot *g_ce_live_cons = NULL;
static ce_live_sector_snapshot *g_ce_live_sectors = NULL;
static ce_live_sector_snapshot *g_ce_live_sector_rows = NULL;
static uint32_t g_ce_live_con_count = 0;
static uint32_t g_ce_live_mapped_system_count = 0;
static uint32_t g_ce_live_sector_count = 0;
static uint32_t g_ce_live_sector_row_count = 0;
static uint32_t g_ce_live_snapshot_galaxy = 0;

/* Planets and stations are not reachable from a single galaxy-level list the
   way stars are (see con_list at galaxy_ptr+0x2c); RScript already walks
   them per-system via StarPlanets()/StarRuins(), so it hands each pointer
   to the DLL one at a time instead. Capture is idempotent per pointer (a
   linear scan against the already-captured list), matching how
   ce_capture_live_arm itself is idempotent per galaxy. */
typedef struct ce_live_named_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
} ce_live_named_snapshot;

#define CE_LIVE_NAMED_CAPACITY 1024u

static ce_live_named_snapshot *g_ce_live_planets = NULL;
static uint32_t g_ce_live_planet_count = 0;
static uint32_t g_ce_live_planet_capacity = 0;

static ce_live_named_snapshot *g_ce_live_stations = NULL;
static uint32_t g_ce_live_station_count = 0;
static uint32_t g_ce_live_station_capacity = 0;

/* The first 11 entries are the canonical anchor systems named in
   MASTER_SPEC_COMPLETE.md / docs/11_SECOND_HOME_GALAXY_MAP.md -- keep
   these exact strings. Everything after was a uniform "Adjective + Noun"
   procedural filler (72/72 entries), which does not match how vanilla
   actually names systems: the decompiled CFG/Rus/Lang.dat's own Star
   table is close to 100% single words, mostly real or invented star
   names ("Тарон", "Ригель", "Антарес", "Арктур", "Велес"), with maybe
   one two-word entry in fifty. Rebuilt the filler the same way. */
static const wchar_t *const g_ce_second_system_names[] = {
    L"Эльтанская Рана", L"Первый Приют", L"Ковчег-IV", L"Крепость Карх",
    L"Узел Без Лица", L"Люмен", L"Призма Единства", L"Пепельный рынок",
    L"Сиротское гнездо", L"Город Незажжённых", L"Врата Второго Дома",
    L"Горн", L"Шлак", L"Уголь", L"Копоть", L"Искра", L"Гарь", L"Зарево",
    L"Пепел", L"Сталь", L"Латунь", L"Ржавчина", L"Окалина", L"Клинок",
    L"Молот", L"Кузнец", L"Горнило", L"Тигель", L"Плавка", L"Литьё",
    L"Сплав", L"Закал", L"Клеймо", L"Оковы", L"Засека", L"Дозор",
    L"Стража", L"Караул", L"Рубеж", L"Порог", L"Предел", L"Грань",
    L"Излом", L"Разлом", L"Раскол", L"Шрам", L"Ожог", L"Пепелище",
    L"Тлен", L"Хмарь", L"Морок", L"Мгла", L"Сумрак", L"Полночь",
    L"Затмение", L"Немота", L"Бездна", L"Провал", L"Утёс", L"Гряда",
    L"Хребет", L"Терраса", L"Уступ", L"Ложбина", L"Впадина", L"Ущелье",
    L"Каньон", L"Затон", L"Плёс", L"Омут", L"Каргаш", L"Тарнов",
    L"Эльтас", L"Агилар", L"Медиан", L"Интель", L"Клиссар", L"Молотар",
    L"Кузнар", L"Наковаль", L"Три Клятвы", L"Стальной Полдень",
    L"Последний Горн",
    /* Two real stars, added on request: both are genuinely reported at
       neighboring-spiral-arm distances from Earth (not just "in that
       constellation's direction" -- checked against sourced distance
       estimates, not assumed). Ро Кассиопеи (Rho Cassiopeiae) is a yellow
       hypergiant at roughly Perseus Arm distance (~8000 ly); Эта Киля (Eta
       Carinae) is placed in the Carina-Sagittarius Arm at ~2.3 kpc. */
    L"Ро Кассиопеи", L"Эта Киля"
};

/* Sector names are NOT rewritten from inside this DLL at all: the sector
   label the map actually draws lives in a generic BlockPar-style config-tree
   "row" object (VMT 0x008273a8, one per Constellations.Name entry, found
   contiguous in memory 0x30 bytes apart) that is completely separate from
   the sector game object tracked below (g_ce_live_sectors) -- finding those
   rows needs a bounded memory scan, and the one in-process scan attempt
   ever tried for this (ce_find_live_sector_rows) correlated with a real
   TGalaxy.NextDay crash and was removed. Doing the exact same kind of scan
   from a separate OS process instead (tools/native/ce_sector_name_helper.c,
   invoked via CreateProcess in ce_spawn_sector_name_helper below) gets the
   same result without ever running on any of this process's own threads,
   so it cannot reproduce that crash. Its own vanilla/Second-Home name pools
   are the single source of truth now; nothing here duplicates them. First
   version of that pool was uniform two-word "Adjective + Noun" (matching
   the same mistake already fixed for stars/planets/stations) -- rebuilt to
   mostly single words once decompiled Constellations.Name showed vanilla's
   20 sector names are 100% single words, zero exceptions. */

/* Planet name pointer offset (+0x14) confirmed via ce_dump_named_object
   live dump: object_ptr+0x14 decoded as a valid immortal Unicode string
   ("Арнарик Гаудад", a real generated planet name) -- not guessed.

   The first version of this pool was uniformly "Adjective + Noun"
   (24/24 entries), which reads nothing like vanilla. Decompiled the real
   CFG/Rus/Lang.dat with BlockParEditor and checked the actual PlanetName
   table the game itself uses: every vanilla planet name is a single
   invented word (varying wildly in length -- "Умий", "Апр", "Заа" vs.
   "Эйманаполон", "Унганамерун"), occasionally hyphenated compounds for
   some races ("Пунэ-анита", "Толт-гаин"). No two-word phrase anywhere in
   that table. Rebuilt this pool the same way, using the project's own
   lore roots (Карх/Тарг/Эльтан/Агилл/Медиум/Интелл/Клиссан) as seeds
   instead of copying vanilla's literal syllables. */
static const wchar_t *const g_ce_second_planet_names[] = {
    L"Карх", L"Таргай", L"Эльтанис", L"Агиллок", L"Медиумар", L"Интеллон",
    L"Клиссен", L"Карходар", L"Таргейн", L"Мельдун", L"Ошган", L"Ривентак",
    L"Зорхим", L"Уннарай", L"Пельтагон", L"Крихой", L"Мандар-Ош",
    L"Тэл-Гуна", L"Орхеста", L"Иннарай", L"Гуртан", L"Веллиш", L"Санторай",
    L"Микдал"
};

/* Station/base name pointer offset (+0x08) confirmed via the same live
   dump: object_ptr+0x08 decoded as a valid immortal Unicode string
   ("Генератор", a real generated station name).

   Same fix as the planet pool: the real vanilla RuinName table (also
   pulled from the decompiled Lang.dat) mixes single evocative words
   ("Толстяк", "Депозит", "Магнат", "Валгалла", "Крюгер") with two-word
   phrases ("Красный барон", "Черная жемчужина", "Пиратская гавань") and
   the rare full phrase ("Заплати и лети"), roughly half and half. This
   pool now does the same instead of "Noun + genitive-noun" on every
   entry. */
static const wchar_t *const g_ce_second_station_names[] = {
    L"Кузня", L"Причал Эльтана", L"Форпост Карха", L"Маяк", L"Верфь Молота",
    L"Застава", L"Санктум", L"Наковальня", L"Пристань Двух Домов",
    L"Клятва", L"Редут Кузнецов", L"Бастион", L"Отражение",
    L"Цитадель Молчания", L"Резонанс", L"Форт Второго Восхода"
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

/* Idempotent per object_ptr (linear scan against what is already captured,
   same style as ce_capture_live_arm's own once-per-galaxy guard). Allocates
   its backing array lazily on first use, capped at CE_LIVE_NAMED_CAPACITY --
   comfortably above any real galaxy's planet/station count, so this never
   grows unbounded or needs realloc. */
static uint32_t ce_capture_named_entity(
        ce_live_named_snapshot **array_ptr, uint32_t *count_ptr, uint32_t *capacity_ptr,
        uint32_t object_ptr, uint32_t name_offset,
        const wchar_t *const *pool, uint32_t pool_size) {
    ce_live_named_snapshot *array;
    uint32_t index;
    uint32_t old_name;
    uint32_t second_name;
    if (object_ptr == 0u) return 0u;
    array = *array_ptr;
    for (index = 0; index < *count_ptr; ++index) {
        if (array[index].object == object_ptr) return 1u;
    }
    if (array == NULL) {
        array = (ce_live_named_snapshot *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            CE_LIVE_NAMED_CAPACITY * sizeof(*array));
        if (array == NULL) return 0u;
        *array_ptr = array;
        *capacity_ptr = CE_LIVE_NAMED_CAPACITY;
    }
    if (*count_ptr >= *capacity_ptr) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(object_ptr + name_offset), 4u, 1))
        return 0u;
    old_name = *(const uint32_t *)(uintptr_t)(object_ptr + name_offset);
    second_name = ce_make_immortal_unicode(pool[*count_ptr % pool_size]);
    if (second_name == 0u) return 0u;
    array[*count_ptr].object = object_ptr;
    array[*count_ptr].old_name = old_name;
    array[*count_ptr].second_name = second_name;
    ++(*count_ptr);
    return 1u;
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

static volatile LONG g_ce_star_con_compared = 0;

/* docs/TGALAXY_LOADFROMSTREAM_FORMAT.md's "Stage 1" (self+0x164, native
   TStar) turned out to be a separate, far more elaborate class than
   "Stage 2" (self+0x2c, native TCon) -- the object this whole session has
   actually been reading/writing as "the star" (name at +0x10, X/Y at
   +0x14/+0x18) is TCon, not TStar; RScript's GalaxyStar()/StarToCon() were
   never touching TStar at all. Before spending more effort fully
   disassembling TStar's large nested format, check empirically whether a
   synthesized second galaxy would even need TStar.count to match
   TCon.count: if the live galaxy's own two counts already differ, they are
   independent and a minimal single-TStar record would be enough
   regardless of how many TCon systems are created. Read-only, bounded,
   one-shot -- same safety class as every other list read this session. */
uint32_t CE_CALL CEAdapterCompareStarConCounts(uint32_t galaxy_ptr) {
    uint32_t tstar_list, tstar_array, tstar_count = 0;
    uint32_t tcon_list, tcon_array, tcon_count = 0;
    char report[192];
    int report_size;
    int tstar_ok, tcon_ok;

    if (InterlockedCompareExchange(&g_ce_star_con_compared, 1, 0) != 0) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x164u), 4u, 0) ||
        !ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x2cu), 4u, 0)) {
        return 0u;
    }
    tstar_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x164u);
    tcon_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    tstar_ok = ce_read_plain_list(tstar_list, &tstar_array, &tstar_count);
    tcon_ok = ce_read_plain_list(tcon_list, &tcon_array, &tcon_count);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"star-con-count-compare\",\"tstar_ok\":%s,\"tstar_count\":%u,"
        "\"tcon_ok\":%s,\"tcon_count\":%u,\"equal\":%s}\r\n",
        tstar_ok ? "true" : "false", tstar_count,
        tcon_ok ? "true" : "false", tcon_count,
        (tstar_ok && tcon_ok && tstar_count == tcon_count) ? "true" : "false");
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return 1u;
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

/* ce_live_row_index_matches and the full-address-space scan that used to
   call it (ce_find_live_sector_rows) were removed: see the comment in
   ce_capture_live_arm where the scan used to run for why -- a synchronous
   VirtualQuery walk of the entire process address space, executed exactly
   once per galaxy on the very first Turn-code invocation, is the prime
   suspect for a reproducible TGalaxy.NextDay crash on turn 0/1 of a new
   game, and the UI-row refresh it fed was cosmetic, not authoritative. */

static int ce_compare_live_sector_objects(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    uint32_t a_id = *(const uint32_t *)(uintptr_t)(a + 4u);
    uint32_t b_id = *(const uint32_t *)(uintptr_t)(b + 4u);
    return a_id < b_id ? -1 : a_id > b_id ? 1 : 0;
}

static int ce_capture_live_arm(uint32_t galaxy_ptr) {
    uint32_t con_list, con_array, con_count;
    uint32_t sector_rows[24];
    uint32_t sector_row_count = 0u;
    uint32_t index;
    ce_live_con_snapshot *new_cons;
    ce_live_sector_snapshot *new_sectors;
    ce_live_sector_snapshot *new_sector_rows = NULL;
    if (g_ce_live_snapshot_galaxy == galaxy_ptr && g_ce_live_cons != NULL) return 1;
    if (g_ce_live_snapshot_galaxy != 0u) {
        /* galaxy_ptr no longer matches what we captured before -- most
           likely the player loaded a different (or the same) save from
           the in-game menu without restarting Rangers.exe, which appears
           to allocate a fresh TGalaxy object rather than reusing the old
           one. A full process restart would have reset every one of these
           globals to zero fresh via DllMain, so this branch specifically
           means "same process, new galaxy" -- the old capture is now
           permanently stale (CEAdapterSetSystemSector/CEAdapterPortalReady
           both gate on an exact galaxy_ptr match, so leaving it as-is
           would silently and permanently break the portal/arm-switch
           feature for the rest of this process's life, which is exactly
           the "anchor stops working after reloading an earlier save" bug
           this fixes). Free the stale snapshot and fall through to
           recapture fresh for the new galaxy. There is no reliable way to
           know which arm should be "active" for a freshly loaded save --
           this mod's arm state is a pure runtime overlay, never persisted
           in the save file itself -- so default back to the old arm, same
           as a fresh process would start. Any in-flight portal state is
           tied to the old (now invalid) galaxy_ptr too and must be
           dropped for the same reason. */
        if (g_ce_live_cons != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_cons);
        if (g_ce_live_sectors != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_sectors);
        if (g_ce_live_sector_rows != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_sector_rows);
        if (g_ce_live_planets != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_planets);
        if (g_ce_live_stations != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_stations);
        g_ce_live_cons = NULL;
        g_ce_live_sectors = NULL;
        g_ce_live_sector_rows = NULL;
        g_ce_live_planets = NULL;
        g_ce_live_stations = NULL;
        g_ce_live_planet_count = 0u;
        g_ce_live_planet_capacity = 0u;
        g_ce_live_station_count = 0u;
        g_ce_live_station_capacity = 0u;
        g_ce_live_con_count = 0u;
        g_ce_live_mapped_system_count = 0u;
        g_ce_live_sector_count = 0u;
        g_ce_live_sector_row_count = 0u;
        g_ce_live_snapshot_galaxy = 0u;
        InterlockedExchange(&g_ce_active_arm, 0);
        InterlockedExchange(&g_ce_portal_status, 0);
        InterlockedExchange(&g_ce_portal_hole_id, 0);
        InterlockedExchange(&g_ce_portal_galaxy_ptr, 0);
        InterlockedExchange(&g_ce_live_switch_lock, 0);
    }
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
    /* ce_find_live_sector_rows used to run here to refresh UI row labels
       for already-open sectors. It is a synchronous full-address-space
       VirtualQuery/byte scan (lpMinimumApplicationAddress through
       lpMaximumApplicationAddress), and it ran exactly once ever per
       galaxy (guarded above) -- squarely on the same first Turn-code
       invocation that reliably crashed the game in TGalaxy.NextDay on a
       brand new save, on every attempt, regardless of which thread/hook
       variant was tried. Its own comment already says it is not
       authoritative (StarToCon() supplies every real TConstellation
       below), so it is cosmetic, not required -- not worth an expensive
       synchronous scan from code that appears to run nested inside
       NextDay's call stack. Skipped entirely; sector_row_count stays 0. */
    sector_row_count = 0u;
    new_cons = (ce_live_con_snapshot *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, con_count * sizeof(*g_ce_live_cons));
    new_sectors = (ce_live_sector_snapshot *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, 24u * sizeof(*g_ce_live_sectors));
    if (sector_row_count != 0u) {
        new_sector_rows = (ce_live_sector_snapshot *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            sector_row_count * sizeof(*g_ce_live_sector_rows));
    }
    if (new_cons == NULL || new_sectors == NULL ||
            (sector_row_count != 0u && new_sector_rows == NULL)) {
        if (new_cons != NULL) HeapFree(GetProcessHeap(), 0, new_cons);
        if (new_sectors != NULL) HeapFree(GetProcessHeap(), 0, new_sectors);
        if (new_sector_rows != NULL) HeapFree(GetProcessHeap(), 0, new_sector_rows);
        return 0;
    }
    for (index = 0; index < con_count; ++index) {
        uint32_t object = *(const uint32_t *)(uintptr_t)(con_array + index * 4u);
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x1cu, 1)) goto capture_fail;
        new_cons[index].object = object;
        new_cons[index].old_name = *(const uint32_t *)(uintptr_t)(object + 0x10u);
        memcpy(&new_cons[index].old_x,
            (const void *)(uintptr_t)(object + 0x14u), sizeof(float));
        memcpy(&new_cons[index].old_y,
            (const void *)(uintptr_t)(object + 0x18u), sizeof(float));
        new_cons[index].second_name = ce_make_immortal_unicode(
            g_ce_second_system_names[index % (sizeof(g_ce_second_system_names) /
                sizeof(g_ce_second_system_names[0]))]);
        if (new_cons[index].second_name == 0u) goto capture_fail;
        new_cons[index].sector_object = 0u;
    }
    for (index = 0; index < sector_row_count; ++index) {
        uint32_t object = sector_rows[index];
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x1cu, 1)) goto capture_fail;
        new_sector_rows[index].object = object;
        new_sector_rows[index].old_name = *(const uint32_t *)(uintptr_t)(object + 0x18u);
        new_sector_rows[index].second_name_index = UINT32_MAX;
    }
    g_ce_live_cons = new_cons;
    g_ce_live_sectors = new_sectors;
    g_ce_live_sector_rows = new_sector_rows;
    g_ce_live_con_count = con_count;
    g_ce_live_mapped_system_count = 0u;
    g_ce_live_sector_count = 0u;
    g_ce_live_sector_row_count = sector_row_count;
    g_ce_live_snapshot_galaxy = galaxy_ptr;
    diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"captured\",\"systems\":%u,\"sector_rows\":%u}\r\n",
        con_count, sector_row_count);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    return 1;

capture_fail:
    for (index = 0u; index < con_count; ++index) {
        if (new_cons[index].second_name != 0u) {
            HeapFree(GetProcessHeap(), 0,
                (void *)(uintptr_t)(new_cons[index].second_name - 8u));
        }
    }
    HeapFree(GetProcessHeap(), 0, new_cons);
    HeapFree(GetProcessHeap(), 0, new_sectors);
    if (new_sector_rows != NULL) HeapFree(GetProcessHeap(), 0, new_sector_rows);
    {
        const char *failure =
            "{\"status\":\"capture-failed\",\"stage\":\"snapshot\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", failure, strlen(failure));
    }
    return 0;
}

static uint32_t ce_live_sector_index(uint32_t object) {
    uint32_t index;
    for (index = 0u; index < g_ce_live_sector_count; ++index) {
        if (g_ce_live_sectors[index].object == object) return index;
    }
    return UINT32_MAX;
}

static int ce_live_names_equal(uint32_t left, uint32_t right) {
    const wchar_t *left_text, *right_text;
    uint32_t left_count, right_count;
    if (left == right) return 1;
    if (!ce_live_read_wide_string(left, &left_text, &left_count) ||
            !ce_live_read_wide_string(right, &right_text, &right_count) ||
            left_count != right_count) return 0;
    return memcmp(left_text, right_text, left_count * sizeof(wchar_t)) == 0;
}

static volatile LONG g_ce_pending_sector_spawn_arm = -1;

/* Launches tools/native/ce_sector_name_helper.exe (built to the same
   directory as this DLL, see build-engine-adapter.ps1) as a completely
   separate OS process, passing our own PID and the target arm, so it can
   rewrite the 20 Constellations.Name "row" objects via its own
   ReadProcessMemory/WriteProcessMemory calls -- see the comment above
   g_ce_second_planet_names for why this has to live outside this process.

   IMPORTANT: this must never be called directly from ce_apply_live_arm (or
   anything else reachable from Turn-code) -- an early version did exactly
   that, reasoning that CreateProcess "returns almost immediately, so it
   cannot reintroduce the loading-stall bug that a blocking Sleep() caused
   right here previously." That reasoning only ruled out the BLOCKING
   variant of the hazard; a live crash on portal entry (matching the exact
   TGalaxy.NextDay crash class already fought earlier in this project --
   see the DllMain/ce_hook_thread_proc comments) showed that the hazard is
   the heavy OS call ITSELF happening on a stack nested inside NextDay, not
   specifically blocking. CreateThread was already known-unsafe there;
   CreateProcess apparently is too. This function must only ever be called
   from ce_sector_spawn_thread_proc, a dedicated thread started once from
   DllMain (never from Turn-code), which polls g_ce_pending_sector_spawn_arm
   instead. ce_apply_live_arm only ever sets that flag (a plain interlocked
   write, no OS call, safe from any call stack) -- see the bottom of that
   function. */
static void ce_spawn_sector_name_helper(int second_home) {
    char module_path[MAX_PATH];
    char command_line[MAX_PATH + 64];
    char *last_slash;
    DWORD length;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;

    length = GetModuleFileNameA(g_ce_adapter_instance, module_path, sizeof(module_path));
    if (length == 0u || length >= sizeof(module_path)) return;
    last_slash = strrchr(module_path, '\\');
    if (last_slash == NULL) return;
    last_slash[1] = '\0';
    if (snprintf(command_line, sizeof(command_line), "\"%sce_sector_name_helper.exe\" %lu %s",
            module_path, (unsigned long)GetCurrentProcessId(),
            second_home ? "second" : "old") < 0) return;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));
    if (CreateProcessA(NULL, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
            NULL, NULL, &startup_info, &process_info)) {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
    }
}

/* Runs forever on its own thread (started once from DllMain, see the
   comment there), completely decoupled from Turn-code/NextDay. Polling
   interval is not latency-sensitive: the sector rename is already a
   one-time visual settle after a portal transition, a few hundred
   milliseconds of extra lag here is unnoticeable. */
static DWORD WINAPI ce_sector_spawn_thread_proc(LPVOID unused) {
    (void)unused;
    for (;;) {
        LONG requested = InterlockedExchange(&g_ce_pending_sector_spawn_arm, -1);
        if (requested >= 0) ce_spawn_sector_name_helper(requested != 0);
        Sleep(75);
    }
}

static void ce_apply_live_arm(int second_home) {
    uint32_t index;
    uint32_t seed = (uint32_t)InterlockedCompareExchange(&g_ce_live_seed, 0, 0);
    float sector_x[24] = {0};
    float sector_y[24] = {0};
    uint32_t sector_members[24] = {0};
    char report[256];
    int report_size;
    if (g_ce_live_cons == NULL || g_ce_live_sectors == NULL ||
            g_ce_live_sector_count == 0u) return;
    qsort(g_ce_live_sectors, g_ce_live_sector_count,
        sizeof(g_ce_live_sectors[0]), ce_compare_live_sector_objects);
    for (index = 0; index < g_ce_live_con_count; ++index) {
        uint32_t sector = ce_live_sector_index(g_ce_live_cons[index].sector_object);
        if (sector < g_ce_live_sector_count) {
            sector_x[sector] += g_ce_live_cons[index].old_x;
            sector_y[sector] += g_ce_live_cons[index].old_y;
            ++sector_members[sector];
        }
    }
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        if (sector_members[index] != 0u) {
            sector_x[index] /= (float)sector_members[index];
            sector_y[index] /= (float)sector_members[index];
        }
    }
    /* A galaxy-center point reflection of each sector's star cluster was
       tried here (mirroring stars to the opposite side of the map) and
       reverted: sector BOUNDARY POLYGONS and their name-label anchor are
       drawn from the sector object's own separately-stored shape data, not
       recomputed from member star positions, so moving only the stars left
       them floating outside/across their sector's unmoved outline -- a
       visibly broken map, reported by the user as "crooked" and matching
       an earlier-seen bug class. Fixing this for real would mean also
       relocating the sector's own polygon data, which needs its own live
       dump investigation (same category as the name-offset hunts, not
       attempted yet) since the sector object's exact shape fields are
       still unknown. Back to jittering within each star's existing sector
       cluster only -- safe, but does not relocate sectors on the map. */
    for (index = 0; index < g_ce_live_con_count; ++index) {
        ce_live_con_snapshot *item = &g_ce_live_cons[index];
        float x = item->old_x;
        float y = item->old_y;
        if (second_home) {
            uint32_t id = *(const uint32_t *)(uintptr_t)(item->object + 4u);
            uint32_t hx = ce_mix32(seed ^ id ^ (index * 0x9e3779b9u));
            uint32_t hy = ce_mix32((seed + 0x85ebca6bu) ^ id ^ (index * 0xc2b2ae35u));
            uint32_t sector = ce_live_sector_index(item->sector_object);
            if (sector < g_ce_live_sector_count && sector_members[sector] != 0u) {
                float dx = item->old_x - sector_x[sector];
                float dy = item->old_y - sector_y[sector];
                float scale = 0.66f + ((float)(hx & 0xffu) / 255.0f) * 0.12f;
                float skew = ((float)((hy >> 8u) & 0xffu) / 255.0f - 0.5f) * 0.16f;
                x = sector_x[sector] + dx * scale - dy * skew;
                y = sector_y[sector] + dy * scale + dx * skew;
            }
        }
        *(uint32_t *)(uintptr_t)(item->object + 0x10u) =
            second_home ? item->second_name : item->old_name;
        memcpy((void *)(uintptr_t)(item->object + 0x14u), &x, sizeof(x));
        memcpy((void *)(uintptr_t)(item->object + 0x18u), &y, sizeof(y));
    }
    /* Sector NAME rewriting never touches this object at all -- see the
       comment above g_ce_second_planet_names for why (+0x0c here is a
       coordinate, not a name pointer; the real label lives in a separate
       config-tree "row" object found and rewritten by an external helper
       process, spawned below via ce_spawn_sector_name_helper). This loop
       only ever runs g_ce_live_sector_row_count times, which stays 0 --
       ce_find_live_sector_rows (the in-process scan that would have
       populated it) is permanently disabled after correlating with a
       crash -- so it is dead code kept only so the row-snapshot capture
       machinery above compiles; it can be removed outright once nothing
       else references g_ce_live_sector_rows. */
    for (index = 0; index < g_ce_live_sector_row_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sector_rows[index];
        uint32_t sector_index;
        uint32_t second_name = 0u;
        for (sector_index = 0u; sector_index < g_ce_live_sector_count; ++sector_index) {
            if (ce_live_names_equal(
                    item->old_name, g_ce_live_sectors[sector_index].old_name)) {
                second_name = g_ce_live_sectors[sector_index].second_name;
                break;
            }
        }
        if (second_name == 0u && index < g_ce_live_sector_count)
            second_name = g_ce_live_sectors[index].second_name;
        *(uint32_t *)(uintptr_t)(item->object + 0x18u) =
            second_home && second_name != 0u ? second_name : item->old_name;
    }
    /* Sector visibility ("explored") flag: bit 0x100 of the dword at
       +0x08, confirmed by a live before/after dump diff (see
       CEAdapterProbeSectorFlagBefore/After) -- every other byte of the
       object was identical before and after SectorVisible(sector,1).
       Unlike SectorVisible() itself, which the documented RScript API can
       only use to OPEN a sector, writing the bit directly can also clear
       it. Entering Second Home closes every sector captured so far (the
       Turn-code that runs right after this opens the arrival sector and
       a couple of neighbors via the normal RScript SectorVisible() call);
       returning to the old arm restores each sector's exact original
       state, captured once in CEAdapterSetSystemSector. The low byte of
       the same dword carries unrelated state and is preserved as-is. */
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sectors[index];
        uint32_t *flag_ptr = (uint32_t *)(uintptr_t)(item->object + 0x08u);
        uint32_t current = *flag_ptr;
        uint32_t want_bit = second_home ? 0u : item->old_visible;
        *flag_ptr = (current & ~0x100u) | want_bit;
    }
    /* Sector name: +0x04 is the 1-based row number into the
       Constellations.Name Lang table (see old_sector_number capture
       comment above). First attempt pointed this at rows 21-44 (this
       mod's own Second Home names, added to that table in
       CE_MapSmoke.Lang.txt) and it had no visible effect in-game --
       sectors kept their vanilla labels while stars/planets/stations
       (same swap-and-restore pattern, different offsets) all renamed
       correctly. Most likely explanation: the map's sector-label drawing
       is native code that resolved Constellations.Name into a fixed-size
       internal table ONCE at startup, sized to the base game's own 20
       rows, and never re-queries the merged Lang.dat by index -- so row
       21+ may simply be unreachable by this mechanism, unlike CT(),
       which RScript queries dynamically by string key. Testing that
       narrower hypothesis before concluding it: permute within the
       KNOWN-valid 1-20 range instead of extending past it. If a sector
       shows a DIFFERENT vanilla name after this, +0x04 does control the
       label and only the >20 extension is the problem; if nothing
       changes even within 1-20, this field is not what draws the label
       at all. Offset +7 (mod 20) is arbitrary, just non-zero. */
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sectors[index];
        uint32_t new_number = ((item->old_sector_number - 1u + 7u) % 20u) + 1u;
        *(uint32_t *)(uintptr_t)(item->object + 0x04u) =
            second_home ? new_number : item->old_sector_number;
    }
    /* Planet (+0x14) and station (+0x08) name-pointer offsets confirmed by
       CEAdapterDumpNamedObject live dumps -- same swap-and-restore shape as
       the star loop above, just per-class offsets. */
    for (index = 0; index < g_ce_live_planet_count; ++index) {
        ce_live_named_snapshot *item = &g_ce_live_planets[index];
        *(uint32_t *)(uintptr_t)(item->object + 0x14u) =
            second_home ? item->second_name : item->old_name;
    }
    for (index = 0; index < g_ce_live_station_count; ++index) {
        ce_live_named_snapshot *item = &g_ce_live_stations[index];
        *(uint32_t *)(uintptr_t)(item->object + 0x08u) =
            second_home ? item->second_name : item->old_name;
    }
    InterlockedExchange(&g_ce_active_arm, second_home ? 1 : 0);
    InterlockedExchange(&g_ce_pending_sector_spawn_arm, second_home ? 1 : 0);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"switched\",\"abi\":%u,\"arm\":\"%s\","
        "\"systems\":%u,\"sectors\":%u,\"planets\":%u,\"stations\":%u}\r\n",
        CEAdapterAbiVersion(), second_home ? "SECOND_HOME" : "OLD_ARM",
        g_ce_live_con_count, g_ce_live_sector_count,
        g_ce_live_planet_count, g_ce_live_station_count);
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

/* F8 is a vanilla quick-save key.  Consume it and forward an otherwise unused
   F24 key to the script UI, so opening the inter-arm portal never also opens
   the vanilla "quick save is absent" dialog. */
/* This used to be a thread-specific WH_KEYBOARD hook, which only ever sees
   keys that pass through GetMessage/PeekMessage on the hooked thread's
   queue. Live testing showed vanilla's own F8 quicksave still fired every
   time regardless -- this engine almost certainly reads its hotkeys from
   polled raw key state (DirectInput-style), not from WM_KEYDOWN messages,
   so swallowing the message never stopped it. WH_KEYBOARD_LL sits at the
   OS-wide low-level input stage, ahead of that polled state entirely:
   returning nonzero here drops the keystroke system-wide before any
   consumer (message queue or polling) ever observes it. */
static void ce_post_virtual_key(HWND window, UINT key) {
    PostMessageW(window, WM_KEYDOWN, key, 1L);
    PostMessageW(window, WM_KEYUP, key, 0xc0000001L);
}

/* The Ctrl+Shift+ELTAN test cheat used to be detected here too, but that
   was solving a problem RScript already solves natively: OnKey exposes
   KEY and KEYMOD (Shift=1, Ctrl=2, Alt=4) straight from the engine, with
   no vanilla binding standing in the way for a plain letter combo (unlike
   F8, which vanilla's own quickload claims before any script ever sees
   it). The cheat now lives entirely in CE_MapSmoke.Main.txt using those,
   which also sidesteps needing this hook to run at all for it. */
static LRESULT CALLBACK ce_portal_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *info = (const KBDLLHOOKSTRUCT *)lparam;
        if (info->vkCode == VK_F8) {
            HWND game_window = GetForegroundWindow();
            DWORD game_pid = 0;
            int is_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
            int is_up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
            GetWindowThreadProcessId(game_window, &game_pid);
            if (game_pid == GetCurrentProcessId()) {
                if (is_down) {
                    if (InterlockedExchange(&g_ce_f8_down, 1) == 0)
                        ce_post_virtual_key(game_window, VK_F24);
                } else if (is_up) {
                    InterlockedExchange(&g_ce_f8_down, 0);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(g_ce_keyboard_hook, code, wparam, lparam);
}

/* Per Microsoft's own docs for WH_KEYBOARD_LL: "This hook is called in the
   context of the thread that installed it ... therefore the thread that
   installed the hook must have a message loop." A thread that only pumps
   messages and installs the hook is required; two earlier variants of
   that thread both crashed the game on turn 0 of a brand new game with a
   native exception in TGalaxy.NextDay:
     1) spawning the thread from CEAdapterInstallLiveArmSwitch and
        blocking the caller on an event until the hook was confirmed;
     2) spawning it fire-and-forget from that same call, without blocking.
   Turn-script code appears to run nested inside NextDay's own call stack,
   so apparently even just calling CreateThread from anywhere reachable
   from it is unsafe. This thread is now started from DllMain instead (see
   the forward declaration above), which fires once at process attach,
   before any galaxy or Turn processing exists -- no connection to
   NextDay's call stack at all. */
static DWORD WINAPI ce_hook_thread_proc(LPVOID unused) {
    MSG msg;
    (void)unused;
    /* A thread has no message queue until it calls a USER32 function that
       needs one; force its creation before installing the hook. */
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    g_ce_keyboard_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL, ce_portal_keyboard_proc, g_ce_adapter_instance, 0);
    if (g_ce_keyboard_hook != NULL) {
        const char *ready = "{\"status\":\"ready\",\"mode\":\"manual-portal\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", ready, strlen(ready));
    }
    if (g_ce_keyboard_hook == NULL) return 1;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

/* The F8 portal only works if VK_F8 is remapped to VK_F24 by the keyboard
   hook installed from DllMain (see ce_hook_thread_proc/ce_portal_keyboard_proc
   above): the script's OnKey handler listens for F24, not F8, precisely so
   the vanilla quicksave binding on F8 stays suppressed. This function used
   to install the hook itself (lazily, the first time it saw galaxy_ptr
   resolve), but every variant of spawning a thread from here -- reachable
   from the Turn script, itself apparently nested inside TGalaxy.NextDay --
   crashed the game on turn 0 of a new game. It now only checks whether the
   DllMain-started thread has finished installing the hook yet. */
uint32_t CE_CALL CEAdapterInstallLiveArmSwitch(uint32_t galaxy_ptr, uint32_t seed) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    InterlockedExchange(&g_ce_live_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_live_seed, (LONG)seed);
    if (g_ce_keyboard_hook == NULL) return 0;
    return ce_capture_live_arm(galaxy_ptr);
}

static volatile LONG g_ce_ssc_logged = 0;
static volatile LONG g_ce_ssc_dumped = 0;

/* One-shot raw dump of the object StarToCon() actually returns, so the
   +0x0c name-field offset (inherited from the removed VMT-scan code,
   which targeted a different discovery path) can be verified or
   corrected against real bytes instead of guessed again. */
static void ce_dump_sector_object(uint32_t sector_ptr) {
    char diagnostic[4096];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"sector-object-dump\",\"sector_ptr\":%u,\"words\":[", sector_ptr);
    if (written < 0) return;
    used = (size_t)written;
    for (offset = 0u; offset < 0x140u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(sector_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(sector_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
}

uint32_t CE_CALL CEAdapterSetSystemSector(
        uint32_t galaxy_ptr, uint32_t system_index, uint32_t sector_ptr) {
    uint32_t sector_index;
    const char *reason = "ok";
    uint32_t result = 1u;

    /* ce_dump_sector_object is a cheap, bounded 0x140-byte read -- safe to
       run inline here. The name-string memory scan is NOT: it is the same
       expensive category of full-address-space walk as the removed
       ce_find_live_sector_rows, which reproduced a real TGalaxy.NextDay
       crash when run synchronously from this Turn-code-reachable path.
       It is started as a background thread from DllMain instead (see
       forward declaration/spawn there), completely decoupled from any
       Turn-code timing. */
    if (sector_ptr != 0u && InterlockedCompareExchange(&g_ce_ssc_dumped, 1, 0) == 0) {
        ce_dump_sector_object(sector_ptr);
    }

    if (g_ce_live_snapshot_galaxy != galaxy_ptr) { reason = "galaxy-mismatch"; result = 0u; }
    else if (g_ce_live_cons == NULL || g_ce_live_sectors == NULL) { reason = "not-captured"; result = 0u; }
    else if (system_index >= g_ce_live_con_count) { reason = "bad-index"; result = 0u; }
    else if (sector_ptr == 0u) { reason = "sector-ptr-zero"; result = 0u; }

    if (result != 0u) {
        sector_index = ce_live_sector_index(sector_ptr);
        if (sector_index == UINT32_MAX) {
            /* The dumped object layout (VMT@+0x00, floats at +0x0c/+0x10,
               a 0x90-byte stride matching a contiguous array) has no
               validated name-string field in its first 0x140 bytes, so
               +0x0c (inherited from the removed VMT-scan code, which
               targeted a different discovery path) is wrong -- it is
               actually a coordinate, and treating it as a name pointer
               would corrupt sector position if ever written back.  Until
               the real name field is found, track sector identity only
               (needed for portal readiness / star clustering); renaming
               is skipped in ce_apply_live_arm below rather than guessed
               again and risk corrupting live galaxy data. */
            if (g_ce_live_sector_count >= 24u) { reason = "sector-table-full"; result = 0u; }
            else if (!ce_region_has_access((const void *)(uintptr_t)sector_ptr, 0x10u, 1)) {
                reason = "sector-unreadable"; result = 0u;
            } else {
                sector_index = g_ce_live_sector_count++;
                g_ce_live_sectors[sector_index].object = sector_ptr;
                g_ce_live_sectors[sector_index].old_name = 0u;
                g_ce_live_sectors[sector_index].second_name = 0u;
                g_ce_live_sectors[sector_index].second_name_index = UINT32_MAX;
                /* Visibility ("explored") flag confirmed live: bit 0x100 of
                   the dword at +0x08 flips from 0 to 1 exactly when RScript
                   calls SectorVisible(sector,1), and nothing else in the
                   object changes (diffed a before/after dump byte-for-byte).
                   The low byte of the same dword is unrelated (already 1
                   before the sector was ever opened) and is preserved, not
                   touched, everywhere this flag is read or written. */
                g_ce_live_sectors[sector_index].old_visible =
                    *(const uint32_t *)(uintptr_t)(sector_ptr + 0x08u) & 0x100u;
                /* Sector "row number" into the vanilla Constellations.Name
                   Lang table: +0x04 tracked sector_index+1 exactly (1..20,
                   zero exceptions) across a live 20-sector survey -- see
                   CEAdapterDumpSectorIndexed. CE_MapSmoke.Lang.txt adds
                   rows 21-44 with this mod's own Second Home sector names
                   under the same Constellations.Name table (Lang.dat
                   sections merge per-key with the base game's, the same
                   way every other CE.* Lang addition in this project
                   already relies on). */
                g_ce_live_sectors[sector_index].old_sector_number =
                    *(const uint32_t *)(uintptr_t)(sector_ptr + 0x04u);
            }
        }
        if (result != 0u) {
            if (g_ce_live_cons[system_index].sector_object == 0u)
                ++g_ce_live_mapped_system_count;
            g_ce_live_cons[system_index].sector_object = sector_ptr;
        }
    }

    if ((uint32_t)InterlockedIncrement(&g_ce_ssc_logged) <= 100u) {
        char diagnostic[160];
        int diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
            "{\"status\":\"set-system-sector\",\"system_index\":%u,"
            "\"sector_ptr\":%u,\"result\":%u,\"reason\":\"%s\"}\r\n",
            system_index, sector_ptr, result, reason);
        if (diagnostic_size > 0) ce_write_text_marker(
            "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    }
    return result;
}

static volatile LONG g_ce_named_dumped[2] = {0, 0};

/* Same bounded, one-shot 0x140-byte read as ce_dump_sector_object, reused
   for planet/station objects so the real name-pointer offset for those
   classes can be confirmed from a live dump instead of assumed. Star
   objects (TCon, from the galaxy's own system list) are already confirmed
   to keep their name pointer at +0x10 (see ce_capture_live_arm/
   ce_apply_live_arm); planets and stations are different classes and may
   or may not share that layout -- this finds out safely, without ever
   scanning unbounded memory. */
static void ce_dump_named_object(const char *kind_label, uint32_t object_ptr) {
    char diagnostic[4096];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"named-object-dump\",\"kind\":\"%s\",\"object_ptr\":%u,\"words\":[",
        kind_label, object_ptr);
    if (written < 0) return;
    used = (size_t)written;
    for (offset = 0u; offset < 0x140u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(object_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(object_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
}

/* kind: 0 = planet (StarPlanets(star,i)), 1 = station (StarRuins(star,i)).
   One-shot per kind, guarded the same way as ce_dump_sector_object -- cheap
   and bounded, safe to call every Turn from the live-arm-switch loop. */
uint32_t CE_CALL CEAdapterDumpNamedObject(uint32_t object_ptr, uint32_t kind) {
    if (object_ptr == 0u || kind > 1u) return 0u;
    if (InterlockedCompareExchange(&g_ce_named_dumped[kind], 1, 0) == 0) {
        ce_dump_named_object(kind == 0u ? "planet" : "station", object_ptr);
    }
    return 1u;
}

static volatile LONG g_ce_ship_snapshot_calls = 0;

/* Same bounded read/string-heuristic dump as ce_dump_named_object, wider
   window (0x400): the player ship (Player(), a TStarShip) is one of the
   larger native object classes touched so far and its item/cargo list is
   not yet known to live within any smaller window already proven safe.
   phase is a caller-supplied tag (0 = before AddItemToShip, 1 = after --
   see the CE_MapSmoke.rson cheat-grant block, which now brackets the
   existing, already-working AddItemToShip call with a snapshot on each
   side) so a single trigger of the cheat produces a matched before/after
   pair in the same marker file, without needing a separately-timed
   external snapshot. Capped at 64 total calls (not gated to one-shot,
   since it needs at least 2 calls per cheat use) purely to keep the log
   file from growing unbounded if the cheat is used repeatedly. */
uint32_t CE_CALL CEAdapterDumpShipSnapshot(uint32_t ship_ptr, uint32_t phase) {
    char diagnostic[16384];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    if (ship_ptr == 0u || (uint32_t)InterlockedIncrement(&g_ce_ship_snapshot_calls) > 64u) return 0u;
    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"ship-snapshot\",\"phase\":%u,\"ship_ptr\":%u,\"words\":[",
        phase, ship_ptr);
    if (written < 0) return 0u;
    used = (size_t)written;
    for (offset = 0u; offset < 0x400u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(ship_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(ship_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
    return 1u;
}

static volatile LONG g_ce_ship_watch_started = 0;
static volatile LONG g_ce_ship_watch_generation = 0;
static uint32_t g_ce_ship_watch_previous[0x100];

static int ce_read_ship_words(uint32_t ship_ptr, uint32_t *words_out) {
    uint32_t offset;
    for (offset = 0u; offset < 0x400u; offset += 4u) {
        if (!ce_region_has_access((const void *)(uintptr_t)(ship_ptr + offset), 4u, 0)) return 0;
        words_out[offset / 4u] = *(const uint32_t *)(uintptr_t)(ship_ptr + offset);
    }
    return 1;
}

/* Passive, generic diff watcher -- unlike CEAdapterDumpShipSnapshot (which
   only fires around this mod's own AddItemToShip call), this catches a
   change caused by ANYTHING, including a vanilla dev cheat (e.g. "ARTS",
   which the user says grants every artifact in the game) that never goes
   through our own Turn-code path at all. Called every Turn from the
   existing "Adapter smoke" block (already runs unconditionally each
   Turn, see CE_MapSmoke.rson); compares the ship's 0x400-byte window
   against what it saw on the previous Turn and, on any difference, dumps
   the full current window plus the exact list of changed byte offsets.
   First call only establishes a baseline (nothing to diff against yet).
   Capped at 64 dumps total, same log-growth reason as
   CEAdapterDumpShipSnapshot. */
uint32_t CE_CALL CEAdapterWatchShipForChanges(uint32_t ship_ptr) {
    uint32_t current[0x100];
    uint32_t changed_offsets[0x100];
    uint32_t changed_count = 0u;
    uint32_t offset;
    char diagnostic[16384];
    int diagnostic_size;
    size_t used = 0;
    int written;
    int first_time;

    if (ship_ptr == 0u || !ce_read_ship_words(ship_ptr, current)) return 0u;
    first_time = InterlockedCompareExchange(&g_ce_ship_watch_started, 1, 0) == 0;
    if (!first_time) {
        for (offset = 0u; offset < 0x100u; ++offset) {
            if (current[offset] != g_ce_ship_watch_previous[offset]) {
                changed_offsets[changed_count++] = offset * 4u;
            }
        }
    }
    memcpy(g_ce_ship_watch_previous, current, sizeof(current));
    if (first_time || changed_count == 0u) return 1u;
    if ((uint32_t)InterlockedIncrement(&g_ce_ship_watch_generation) > 64u) return 1u;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"ship-watch-changed\",\"ship_ptr\":%u,\"changed_count\":%u,\"changed_offsets\":[",
        ship_ptr, changed_count);
    if (written < 0) return 1u;
    used = (size_t)written;
    for (offset = 0u; offset < changed_count && used + 16u < sizeof(diagnostic); ++offset) {
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s%u", offset == 0u ? "" : ",", changed_offsets[offset]);
        if (written > 0) used += (size_t)written;
    }
    written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "],\"words\":[");
    if (written > 0) used += (size_t)written;
    for (offset = 0u; offset < 0x100u && used + 96u < sizeof(diagnostic); ++offset) {
        uint32_t value = current[offset];
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (value >= 0x10000u &&
                ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
            uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
            if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                    ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                preview_len = byte_length / 2u;
                if (preview_len > 20u) preview_len = 20u;
                memcpy(preview, text, preview_len * sizeof(wchar_t));
                has_string = 1;
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset * 4u, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
    return 1u;
}

static volatile LONG g_ce_sector_survey_started = 0;

/* Same bounded-read/string-heuristic shape as ce_dump_named_object, but a
   wider window (0x300 instead of 0x140): the sector's name is confirmed
   absent from the first 0x140 bytes, so this checks further out in case
   an embedded name pointer exists past where the earlier dump stopped.
   Tags each entry with the caller-supplied index (GalaxySectors(i)'s i,
   a plain dword -- no string marshaling needed) so it can be matched
   against the sector's real name, displayed separately via Ether() from
   RScript using ConName(), which needs no new DLL surface at all. */
static void ce_dump_sector_indexed(uint32_t sector_ptr, uint32_t index) {
    char diagnostic[12288];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"sector-survey\",\"sector_index\":%u,\"object_ptr\":%u,\"words\":[",
        index, sector_ptr);
    if (written < 0) return;
    used = (size_t)written;
    for (offset = 0u; offset < 0x300u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(sector_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(sector_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
}

/* Gate for a one-shot full-galaxy sector survey: returns 1 exactly once
   (ever), so RScript can wrap a "loop every sector, dump + announce its
   name" block in it without needing per-call dedup bookkeeping. Bounded,
   single-object reads per sector (same safe category as every other dump
   this session) -- not a memory scan. */
uint32_t CE_CALL CEAdapterBeginSectorSurvey(void) {
    return (uint32_t)(InterlockedCompareExchange(&g_ce_sector_survey_started, 1, 0) == 0);
}

uint32_t CE_CALL CEAdapterDumpSectorIndexed(uint32_t sector_ptr, uint32_t index) {
    if (sector_ptr == 0u) return 0u;
    ce_dump_sector_indexed(sector_ptr, index);
    return 1u;
}

static volatile LONG g_ce_cluster_survey_started = 0;

/* TGALAXY_LOADFROMSTREAM_FORMAT.md documents a disassembled "Stage 3"
   class (candidate: sectors/TCluster), stored in the galaxy's own list at
   self+0x34 (same list shape as the already-proven star list at
   self+0x2c), whose init function's FIRST action is to read a STRING via
   the documented string-reader (0x0082ff18) straight into its own
   self+0x04 -- i.e. a real embedded name pointer, confirmed by disassembly
   of Rangers.exe, not a live-memory guess. This cannot be the same class
   GalaxySectors()/StarToCon() return: those objects' own +0x04 is a small
   sequential integer (1..20, already tested as a write target with no
   effect on the displayed name), not a valid heap pointer. Whatever
   GalaxySectors() exposes to RScript is most likely a lightweight runtime
   view (TConstellation) distinct from this raw TCluster save-data class.
   Reads galaxy_ptr+0x34 with the exact same ce_read_plain_list() call
   already proven safe for the star list, then bounded-dumps each member
   the same way as every other object this session -- no full-memory scan,
   no new offset guessed without a documented basis. */
uint32_t CE_CALL CEAdapterSurveyClusterList(uint32_t galaxy_ptr) {
    uint32_t cluster_list, cluster_array, cluster_count, i;
    char diagnostic[256];
    int diagnostic_size;
    if (InterlockedCompareExchange(&g_ce_cluster_survey_started, 1, 0) != 0) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x34u), 4u, 0)) {
        const char *failure = "{\"status\":\"cluster-survey-failed\",\"stage\":\"galaxy-field\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", failure, strlen(failure));
        return 0u;
    }
    cluster_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x34u);
    if (!ce_read_plain_list(cluster_list, &cluster_array, &cluster_count)) {
        const char *failure = "{\"status\":\"cluster-survey-failed\",\"stage\":\"list\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", failure, strlen(failure));
        return 0u;
    }
    diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"cluster-survey-count\",\"count\":%u}\r\n", cluster_count);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    for (i = 0; i < cluster_count && i < 24u; ++i) {
        uint32_t obj;
        if (!ce_region_has_access((const void *)(uintptr_t)(cluster_array + i * 4u), 4u, 0)) break;
        obj = *(const uint32_t *)(uintptr_t)(cluster_array + i * 4u);
        if (obj != 0u) ce_dump_sector_indexed(obj, i);
    }
    return 1u;
}

/* +0x04 tracks sector_index+1 across all 20 captured sectors, with zero
   exceptions -- a live before/after write test is the only way to know
   whether this is really the 1-based row number into Constellations.Name
   (a hypothesis from the decompiled vanilla Lang.dat, which has exactly a
   20-row Constellations.Name table matching this galaxy's 20 sectors) or
   just an unrelated creation-order counter. Bounded single-dword write,
   same risk class as every other confirmed offset this session. */
uint32_t CE_CALL CEAdapterSetSectorIndex(uint32_t sector_ptr, uint32_t new_index) {
    if (sector_ptr == 0u) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(sector_ptr + 4u), 4u, 1)) return 0u;
    *(uint32_t *)(uintptr_t)(sector_ptr + 4u) = new_index;
    return 1u;
}

static volatile LONG g_ce_sector_flag_probed = 0;

/* SectorVisible() can only open a sector via documented RScript, never
   close one -- if the "explored" flag turns out to be a plain byte/dword
   on the sector object (same category as the name-pointer offsets already
   found this way), a direct write could close it too. Find the flag by
   dumping the SAME sector's bytes immediately before and after RScript
   calls SectorVisible(sector,1) on it, then diffing by eye in the log --
   one-shot, fires on the first currently-unexplored sector it finds, and
   deliberately opens exactly that one sector as a side effect (RScript
   only calls this pair around a real SectorVisible(...,1) call). */
uint32_t CE_CALL CEAdapterProbeSectorFlagBefore(uint32_t sector_ptr) {
    if (sector_ptr == 0u) return 0u;
    if (InterlockedCompareExchange(&g_ce_sector_flag_probed, 1, 0) != 0) return 0u;
    ce_dump_named_object("sector-before-open", sector_ptr);
    return 1u;
}

uint32_t CE_CALL CEAdapterProbeSectorFlagAfter(uint32_t sector_ptr) {
    if (sector_ptr == 0u) return 0u;
    ce_dump_named_object("sector-after-open", sector_ptr);
    return 1u;
}

/* Name pointer offset (+0x14) confirmed by CEAdapterDumpNamedObject's live
   dump, not guessed -- see g_ce_second_planet_names above. */
uint32_t CE_CALL CEAdapterCapturePlanetName(uint32_t galaxy_ptr, uint32_t planet_ptr) {
    if (g_ce_live_snapshot_galaxy != galaxy_ptr) return 0u;
    return ce_capture_named_entity(&g_ce_live_planets, &g_ce_live_planet_count,
        &g_ce_live_planet_capacity, planet_ptr, 0x14u,
        g_ce_second_planet_names,
        (uint32_t)(sizeof(g_ce_second_planet_names) / sizeof(g_ce_second_planet_names[0])));
}

/* Name pointer offset (+0x08) confirmed by CEAdapterDumpNamedObject's live
   dump, not guessed -- see g_ce_second_station_names above. */
uint32_t CE_CALL CEAdapterCaptureStationName(uint32_t galaxy_ptr, uint32_t station_ptr) {
    if (g_ce_live_snapshot_galaxy != galaxy_ptr) return 0u;
    return ce_capture_named_entity(&g_ce_live_stations, &g_ce_live_station_count,
        &g_ce_live_station_capacity, station_ptr, 0x08u,
        g_ce_second_station_names,
        (uint32_t)(sizeof(g_ce_second_station_names) / sizeof(g_ce_second_station_names[0])));
}

/* star_owner_value is whatever RScript's own StarOwner(star) call just
   returned (1 == Dominators, per the documented 0=Coalition/1=Dominators/
   2=Pirates scale) -- this DLL has no way to call that built-in itself.
   Idempotent per system_index (first value seen wins) so the captured
   baseline always reflects the true old-arm state, even though the
   per-star Turn-code loop that calls this keeps running every turn
   afterwards, including while Second Home's own suppression is active. */
uint32_t CE_CALL CEAdapterCaptureDominatorFlag(uint32_t system_index, uint32_t star_owner_value) {
    if (g_ce_live_cons == NULL || system_index >= g_ce_live_con_count) return 0u;
    if (g_ce_live_cons[system_index].dominator_captured == 0u) {
        g_ce_live_cons[system_index].old_dominator = star_owner_value == 1u ? 1u : 0u;
        g_ce_live_cons[system_index].dominator_captured = 1u;
    }
    return 1u;
}

uint32_t CE_CALL CEAdapterWasDominator(uint32_t system_index) {
    if (g_ce_live_cons == NULL || system_index >= g_ce_live_con_count) return 0u;
    return g_ce_live_cons[system_index].old_dominator;
}

static volatile LONG g_ce_dominator_count_logged = 0;

/* One-shot diagnostic: dominators are still visible after 0.0.33's fix
   despite the per-turn sweep confirmed running (ship-watch-changed fired
   repeatedly during the Second Home visit). Prime suspect: the dominator
   flag capture -- gated behind ce_capture_live_arm's "first Turn tick
   ever" idempotent guard, same as planet/station name capture -- may run
   too early (before the engine has finished computing real conquest
   state after a save load), permanently freezing every system's captured
   flag at 0 (Coalition) regardless of true ownership. This counts how
   many of the captured systems actually show old_dominator==1, logged
   once so it can be checked without guessing further. */
uint32_t CE_CALL CEAdapterLogDominatorFlagCount(void) {
    uint32_t index, count = 0u;
    char payload[96];
    int size;
    if (InterlockedCompareExchange(&g_ce_dominator_count_logged, 1, 0) != 0) return 0u;
    if (g_ce_live_cons != NULL) {
        for (index = 0; index < g_ce_live_con_count; ++index) {
            if (g_ce_live_cons[index].old_dominator != 0u) ++count;
        }
    }
    size = snprintf(payload, sizeof(payload),
        "{\"status\":\"dominator-flag-count\",\"count\":%u,\"total\":%u}\r\n",
        count, g_ce_live_con_count);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
    return 1u;
}

typedef struct ce_hidden_boss_entry {
    uint32_t ship_ptr;
    uint32_t original_star_ptr;
} ce_hidden_boss_entry;

#define CE_HIDDEN_BOSS_CAPACITY 32u
static ce_hidden_boss_entry g_ce_hidden_bosses[CE_HIDDEN_BOSS_CAPACITY];
static uint32_t g_ce_hidden_boss_count = 0u;

/* Remembers a hidden boss/station ship's original star as a plain raw
   pointer in this DLL's own memory -- the same proven storage pattern
   used throughout this project (g_ce_live_cons etc.), deliberately NOT
   using RScript's ShipAddCustomShipInfo/ShipCustomShipInfoData for this:
   that mechanism's numeric data slots are documented as plain "int"
   values (e.g. CurTurn()+Rnd(...) in every real reference-mod example
   found), never as a native object reference -- storing a raw star
   pointer through it was an untested assumption this project's own
   practice is to avoid shipping without verification. Idempotent per
   ship_ptr (overwrites if already present) so a ship hidden more than
   once (e.g. re-entering Second Home before ever returning) does not
   grow the table or leak an entry. */
uint32_t CE_CALL CEAdapterHideBossShip(uint32_t ship_ptr, uint32_t original_star_ptr) {
    uint32_t index;
    if (ship_ptr == 0u || original_star_ptr == 0u) return 0u;
    for (index = 0; index < g_ce_hidden_boss_count; ++index) {
        if (g_ce_hidden_bosses[index].ship_ptr == ship_ptr) {
            g_ce_hidden_bosses[index].original_star_ptr = original_star_ptr;
            return 1u;
        }
    }
    if (g_ce_hidden_boss_count >= CE_HIDDEN_BOSS_CAPACITY) return 0u;
    g_ce_hidden_bosses[g_ce_hidden_boss_count].ship_ptr = ship_ptr;
    g_ce_hidden_bosses[g_ce_hidden_boss_count].original_star_ptr = original_star_ptr;
    ++g_ce_hidden_boss_count;
    return 1u;
}

/* Returns the remembered original star for ship_ptr (0 if none), and
   removes the entry (swap-with-last) so it is only ever consumed once. */
uint32_t CE_CALL CEAdapterRestoreBossShip(uint32_t ship_ptr) {
    uint32_t index;
    for (index = 0; index < g_ce_hidden_boss_count; ++index) {
        if (g_ce_hidden_bosses[index].ship_ptr == ship_ptr) {
            uint32_t original_star = g_ce_hidden_bosses[index].original_star_ptr;
            g_ce_hidden_bosses[index] = g_ce_hidden_bosses[g_ce_hidden_boss_count - 1u];
            --g_ce_hidden_boss_count;
            return original_star;
        }
    }
    return 0u;
}

/* Same idea as CEAdapterRestoreBossShip but for the restore-side loop,
   which does not already know each ship_ptr up front -- iterates whatever
   is currently in the table by position instead. Returns 0 once index is
   past the end (safe loop terminator for RScript's while-loop style). */
uint32_t CE_CALL CEAdapterHiddenBossCount(void) {
    return g_ce_hidden_boss_count;
}

uint32_t CE_CALL CEAdapterHiddenBossShipAt(uint32_t index) {
    if (index >= g_ce_hidden_boss_count) return 0u;
    return g_ce_hidden_bosses[index].ship_ptr;
}

/* Fine-grained progress marker for live bisection without needing another
   rebuild-test round-trip: writes {"status":"checkpoint","id":N} to the
   debug log. Placed at each major step of the arm-switch-completion
   Dominator/boss handling in CE_MapSmoke.rson so that if the game crashes
   again, the log's last checkpoint id pinpoints exactly which step was
   reached, the same way the project's own proven "strip Turn-code down,
   add pieces back" bisection technique works, but in one pass instead of
   several rebuild cycles. */
uint32_t CE_CALL CEAdapterLogCheckpoint(uint32_t id) {
    char payload[64];
    int size = snprintf(payload, sizeof(payload), "{\"status\":\"checkpoint\",\"id\":%u}\r\n", id);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
    return 1u;
}

/* Two independent argument changes to the BuildListOfNewShips call site
   (sinceId 0->1, then typeSet/raceSet swapped to the working t_Kling
   filter) both still crashed at the exact same checkpoint, with the crash
   never reaching just past the call -- pointing at WHEN it is called
   (nested in NextDay's own call stack, on the exact Turn the portal
   transition just completed) rather than WHAT arguments it is given.
   Deferring the sweep to the very next unconditional per-Turn tick (the
   "Adapter smoke" block, which fires far more often than once per
   calendar day during normal flight, so the delay is imperceptible)
   moves the call off that uniquely fragile moment -- the same fix shape
   already proven for ce_spawn_sector_name_helper's CreateProcess call. */
static volatile LONG g_ce_pending_ship_sweep_arm = -1;

uint32_t CE_CALL CEAdapterSetPendingShipSweep(uint32_t second_home) {
    InterlockedExchange(&g_ce_pending_ship_sweep_arm, second_home != 0u ? 1 : 0);
    return 1u;
}

/* Returns 2 if nothing is pending (RScript-friendly sentinel distinct
   from the real 0/1 arm values), consuming the flag on any non-empty
   read so it only ever fires once per arm switch. */
uint32_t CE_CALL CEAdapterConsumePendingShipSweep(void) {
    LONG value = InterlockedExchange(&g_ce_pending_ship_sweep_arm, -1);
    return value < 0 ? 2u : (uint32_t)value;
}

uint32_t CE_CALL CEAdapterPortalReady(uint32_t galaxy_ptr) {
    uint32_t ready = g_ce_live_snapshot_galaxy == galaxy_ptr && g_ce_live_cons != NULL &&
        g_ce_live_sectors != NULL && g_ce_live_sector_count != 0u &&
        g_ce_live_mapped_system_count == g_ce_live_con_count &&
        InterlockedCompareExchange(&g_ce_portal_status, 0, 0) == 0 ? 1u : 0u;
    char diagnostic[256];
    int diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"portal-ready-check\",\"ready\":%s,\"snapshot_matches\":%s,"
        "\"cons_count\":%u,\"mapped_count\":%u,\"sector_count\":%u,"
        "\"portal_status\":%ld}\r\n",
        ready ? "true" : "false",
        g_ce_live_snapshot_galaxy == galaxy_ptr ? "true" : "false",
        g_ce_live_con_count, g_ce_live_mapped_system_count, g_ce_live_sector_count,
        (long)InterlockedCompareExchange(&g_ce_portal_status, 0, 0));
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    return ready;
}

uint32_t CE_CALL CEAdapterPortalStatus(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_portal_status, 0, 0);
}

/* KEY/KEYMOD come straight from RScript's OnKey (Shift=1, Ctrl=2, Alt=4);
   no reference mod calls ShipAddCustomShipInfo or any other engine-mutating
   API from interface code (OnKey/OnPressCode) -- every real example is
   from act-code or a dialog handler, which fire far less often and never
   from three duplicate panels at once. This function itself does nothing
   but advance a plain counter (safe to call from OnKey on every keystroke,
   even 2-3 times per key from duplicate panels); the actual item grant
   still runs from the Turn-code poll in CE_MapSmoke.rson, the same
   proven-safe context every other heavy call in this mod uses -- that part
   is unchanged.

   What changed: this used to always return 0, leaving the player waiting
   for a real day to pass before Turn-code ever re-ran and noticed the
   trigger. Confirmed via the decompiled RScript function reference
   (references\Universe-Original-Mods-Source\Script functions list.txt,
   "Ход (даты)" section, right next to CurTurn -- a function this mod
   already calls elsewhere) that ForceNextDay() is a real, documented,
   no-argument RScript function that forces the next Turn to run
   immediately. Now returns 1 exactly on the keystroke that completes the
   sequence, so CE_MapSmoke.Main.txt can call ForceNextDay() right there --
   still never touching ship/cargo state itself from OnKey, just nudging
   the clock forward so the ALREADY-safe Turn-code path (which does the
   real mutation) fires now instead of whenever the player next skips a
   day. This still needs a real live-game check before it can be trusted:
   TGalaxy.NextDay is the exact code path that has crashed this project
   multiple times before when entered in a nested/reentrant way (see the
   DllMain/ce_hook_thread_proc comments), and OnKey firing from three
   duplicate panels at once means ForceNextDay could plausibly be invoked
   more than once for a single keystroke. */
uint32_t CE_CALL CEAdapterAdvanceCheatCode(uint32_t key, uint32_t keymod) {
    static const uint32_t cheat[] = {'E', 'L', 'T', 'A', 'N'};
    char diagnostic[128];
    int diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"cheat-key\",\"key\":%u,\"keymod\":%u,\"progress_before\":%u}\r\n",
        key, keymod, g_ce_cheat_progress);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    if (keymod != 3u || key < 'A' || key > 'Z') return 0u;
    if (key == cheat[g_ce_cheat_progress]) {
        ++g_ce_cheat_progress;
    } else {
        g_ce_cheat_progress = key == cheat[0] ? 1u : 0u;
    }
    if (g_ce_cheat_progress == sizeof(cheat) / sizeof(cheat[0])) {
        g_ce_cheat_progress = 0u;
        InterlockedExchange(&g_ce_cheat_triggered, 1);
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"cheat-triggered\"}\r\n", 30u);
        return 1u;
    }
    return 0u;
}

uint32_t CE_CALL CEAdapterConsumeCheatTrigger(void) {
    return (uint32_t)InterlockedExchange(&g_ce_cheat_triggered, 0);
}

uint32_t CE_CALL CEAdapterRegisterPortal(uint32_t galaxy_ptr, uint32_t hole_id) {
    char report[192];
    int report_size;
    if (hole_id == 0u || !CEAdapterPortalReady(galaxy_ptr) ||
            InterlockedCompareExchange(&g_ce_portal_status, 1, 0) != 0) return 0;
    InterlockedExchange(&g_ce_portal_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_portal_hole_id, (LONG)hole_id);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"portal-opened\",\"hole_id\":%u,\"arm\":\"%s\"}\r\n",
        hole_id, InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0
            ? "OLD_ARM" : "SECOND_HOME");
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return 1;
}

uint32_t CE_CALL CEAdapterEnterRegisteredPortal(uint32_t galaxy_ptr, uint32_t hole_id) {
    char report[192];
    int report_size;
    if (hole_id == 0u ||
            (uint32_t)InterlockedCompareExchange(&g_ce_portal_hole_id, 0, 0) != hole_id ||
            (uint32_t)InterlockedCompareExchange(&g_ce_portal_galaxy_ptr, 0, 0) != galaxy_ptr ||
            InterlockedCompareExchange(&g_ce_portal_status, 2, 1) != 1) return 0;
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"portal-entered\",\"hole_id\":%u,\"phase\":\"awaiting-exit\"}\r\n",
        hole_id);
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return 1;
}

/* No sleep and no custom overlay window here anymore: this used to show a
   topmost "Резонансный переход" window and Sleep(700)+Sleep(450) (over a
   second of blocking) around the actual rename. That window sat on top of
   the game's own native loading screen and the sleeps blocked whatever
   thread the native loader needed, which reproduced a real hang at ~15%
   loading when entering the hole. ce_apply_live_arm itself is a fast,
   pure in-memory rewrite (no I/O) -- it needs no artificial pause or
   visual cover, so it just runs immediately. */
uint32_t CE_CALL CEAdapterCompleteRegisteredPortal(uint32_t galaxy_ptr) {
    char report[192];
    int report_size;
    if ((uint32_t)InterlockedCompareExchange(&g_ce_portal_galaxy_ptr, 0, 0) != galaxy_ptr ||
            InterlockedCompareExchange(&g_ce_portal_status, 3, 2) != 2) return 0;
    if (InterlockedCompareExchange(&g_ce_live_switch_lock, 1, 0) != 0) {
        InterlockedExchange(&g_ce_portal_status, 2);
        return 0;
    }
    ce_apply_live_arm(InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0);
    EnumWindows(ce_invalidate_process_window, 0);
    InterlockedExchange(&g_ce_portal_hole_id, 0);
    InterlockedExchange(&g_ce_portal_galaxy_ptr, 0);
    InterlockedExchange(&g_ce_portal_status, 0);
    InterlockedExchange(&g_ce_live_switch_lock, 0);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"portal-complete\",\"arm\":\"%s\"}\r\n",
        InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0
            ? "OLD_ARM" : "SECOND_HOME");
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
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
