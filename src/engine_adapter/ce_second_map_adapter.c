#include "ce_second_map_adapter.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

static volatile LONG g_ce_galaxy_ptr = 0;
static volatile LONG g_ce_fingerprint_hash = 0;
static volatile LONG g_ce_fingerprint_bytes = 0;
static volatile LONG g_ce_layout_written = 0;
static volatile LONG g_ce_layout_block_hash[4] = {0, 0, 0, 0};
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

uint32_t CE_CALL CEAdapterAbiVersion(void) {
    return CE_ADAPTER_ABI_VERSION;
}

uint32_t CE_CALL CEAdapterCapabilities(void) {
    return CE_CAP_BIND_GALAXY_POINTER | CE_CAP_SMOKE_MARKER |
        CE_CAP_READONLY_GALAXY_FINGERPRINT |
        CE_CAP_READONLY_GALAXY_LAYOUT_SAMPLE |
        CE_CAP_READONLY_GALAXY_LAYOUT_LATEST;
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
    MEMORY_BASIC_INFORMATION memory;
    SIZE_T bytes_read = 0;
    uintptr_t address = (uintptr_t)galaxy_ptr;
    uintptr_t region_start;
    uintptr_t region_end;
    uint32_t block_hash[4];
    uint32_t zero_low = 0;
    uint32_t zero_high = 0;
    uint32_t pointer_low = 0;
    uint32_t pointer_high = 0;
    uint32_t index;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[768];
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
    for (index = 0; index < 4; ++index) {
        block_hash[index] = ce_fnv1a32(sample + index * 64u, 64u);
        InterlockedExchange(&g_ce_layout_block_hash[index], (LONG)block_hash[index]);
    }
    for (index = 0; index < 64; ++index) {
        uint32_t word;
        uint32_t bit = 1u << (index & 31u);
        memcpy(&word, sample + index * sizeof(word), sizeof(word));
        if (word == 0) {
            if (index < 32) zero_low |= bit; else zero_high |= bit;
        } else if (ce_is_readable_pointer(word)) {
            if (index < 32) pointer_low |= bit; else pointer_high |= bit;
        }
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
        "\"readable_pointer_mask\":\"%08lX%08lX\",\"raw_values_included\":false,"
        "\"read_only\":true}\r\n",
        CEAdapterAbiVersion(), (unsigned long)GetCurrentProcessId(), sample_tag,
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_tag, 0, 0),
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_sequence, 0, 0),
        block_hash[0], block_hash[1], block_hash[2], block_hash[3],
        (unsigned long)zero_high, (unsigned long)zero_low,
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
    return 0;
}
