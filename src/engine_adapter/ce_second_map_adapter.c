#include "ce_second_map_adapter.h"

#include <windows.h>

#include <stdio.h>

static volatile LONG g_ce_galaxy_ptr = 0;
static volatile LONG g_ce_fingerprint_hash = 0;
static volatile LONG g_ce_fingerprint_bytes = 0;

uint32_t CE_CALL CEAdapterAbiVersion(void) {
    return CE_ADAPTER_ABI_VERSION;
}

uint32_t CE_CALL CEAdapterCapabilities(void) {
    return CE_CAP_BIND_GALAXY_POINTER | CE_CAP_SMOKE_MARKER |
        CE_CAP_READONLY_GALAXY_FINGERPRINT;
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
        "{\"abi\":%u,\"galaxy_ptr\":%u,\"sample_bytes\":%u,"
        "\"fnv1a32\":%u,\"region_base\":%u,\"region_size\":%lu,"
        "\"state\":%lu,\"protect\":%lu,\"type\":%lu,\"read_only\":true}\r\n",
        CEAdapterAbiVersion(), galaxy_ptr, byte_count, hash,
        (uint32_t)region_start, (unsigned long)memory.RegionSize,
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

uint32_t CE_CALL CEAdapterSupportsNativeMultiGalaxy(void) {
    return 0;
}
