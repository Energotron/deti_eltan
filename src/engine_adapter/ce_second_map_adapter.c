#include "ce_second_map_adapter.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

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

/* Steam build 20648864 / Rangers.exe 2.1.2500.0 only. */
enum {
    CE_RANGERS_TIMESTAMP = 0x68eccc46u,
    CE_RANGERS_IMAGE_SIZE = 0x004d1000u,
    CE_RVA_GALAXY_IMPORT_CELL = 0x0048263cu,
    CE_RVA_TGALAXY_CLASS_CELL = 0x00438d90u,
    CE_RVA_TGALAXY_CONSTRUCTOR = 0x00439198u,
    CE_RVA_TGALAXY_INITIALIZE = 0x0043a034u,
    CE_RVA_TGALAXY_GENERATE_STARS = 0x0044ff74u
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
        CE_CAP_EXPERIMENTAL_ENGINE_GALAXY;
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
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    *galaxy_slot = second_galaxy;
    InterlockedExchange(&g_ce_active_arm, 1);
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
