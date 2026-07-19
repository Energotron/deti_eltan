#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CE_SAMPLE_BYTES 256u
#define CE_DWORD_COUNT 64u
#define CE_CHUNK_BYTES (1024u * 1024u)
#define CE_MAX_REPORTED 64u

/* Intersection of the confirmed ABI 4 and ABI 5 observations. */
#define CE_STABLE_ZERO_MASK UINT64_C(0x0818404000000400)
#define CE_STABLE_POINTER_MASK UINT64_C(0x000300003000F801)

typedef struct ce_candidate {
    uint32_t hashes[4];
    uint64_t zero_mask;
    uint64_t pointer_mask;
} ce_candidate;

typedef struct ce_scan_result {
    uint64_t regions;
    uint64_t bytes_scanned;
    uint64_t candidate_count;
    ce_candidate reported[CE_MAX_REPORTED];
    size_t reported_count;
} ce_scan_result;

static int ce_is_readable(DWORD protect) {
    DWORD base = protect & 0xffu;
    if ((protect & PAGE_GUARD) != 0 || base == PAGE_NOACCESS) return 0;
    return base == PAGE_READONLY || base == PAGE_READWRITE ||
        base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

static uint32_t ce_read_u32(const unsigned char *data) {
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static uint32_t ce_fnv1a32(const unsigned char *data, size_t size) {
    uint32_t hash = UINT32_C(2166136261);
    size_t index;
    for (index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int ce_is_readable_pointer(HANDLE process, uint32_t value) {
    MEMORY_BASIC_INFORMATION memory;
    uintptr_t address = (uintptr_t)value;
    if (value < UINT32_C(0x10000) || (value & 3u) != 0) return 0;
    if (VirtualQueryEx(process, (LPCVOID)address, &memory, sizeof(memory)) !=
            sizeof(memory)) return 0;
    return memory.State == MEM_COMMIT && ce_is_readable(memory.Protect);
}

static uint64_t ce_zero_mask(const unsigned char *sample) {
    uint64_t result = 0;
    unsigned index;
    for (index = 0; index < CE_DWORD_COUNT; ++index) {
        if (ce_read_u32(sample + index * 4u) == 0) result |= UINT64_C(1) << index;
    }
    return result;
}

static int ce_required_pointers_match(
    HANDLE process, const unsigned char *sample, uint64_t required
) {
    unsigned index;
    for (index = 0; index < CE_DWORD_COUNT; ++index) {
        if ((required & (UINT64_C(1) << index)) != 0 &&
                !ce_is_readable_pointer(process, ce_read_u32(sample + index * 4u))) {
            return 0;
        }
    }
    return 1;
}

static uint64_t ce_pointer_mask(HANDLE process, const unsigned char *sample) {
    uint64_t result = 0;
    unsigned index;
    for (index = 0; index < CE_DWORD_COUNT; ++index) {
        if (ce_is_readable_pointer(process, ce_read_u32(sample + index * 4u))) {
            result |= UINT64_C(1) << index;
        }
    }
    return result;
}

static int ce_match_sample(
    HANDLE process,
    const unsigned char *sample,
    uint64_t expected_zero,
    uint64_t expected_pointer,
    int exact,
    ce_candidate *candidate
) {
    uint64_t zeros = ce_zero_mask(sample);
    uint64_t pointers;
    unsigned block;
    if ((zeros & expected_zero) != expected_zero) return 0;
    if (exact && zeros != expected_zero) return 0;
    if (!ce_required_pointers_match(process, sample, expected_pointer)) return 0;
    pointers = ce_pointer_mask(process, sample);
    if (exact && pointers != expected_pointer) return 0;
    if ((pointers & expected_pointer) != expected_pointer) return 0;
    for (block = 0; block < 4; ++block) {
        candidate->hashes[block] = ce_fnv1a32(sample + block * 64u, 64u);
    }
    candidate->zero_mask = zeros;
    candidate->pointer_mask = pointers;
    return 1;
}

static void ce_record_candidate(ce_scan_result *result, const ce_candidate *candidate) {
    ++result->candidate_count;
    if (result->reported_count < CE_MAX_REPORTED) {
        result->reported[result->reported_count++] = *candidate;
    }
}

static int ce_scan_region(
    HANDLE process,
    uintptr_t base,
    SIZE_T region_size,
    uint64_t expected_zero,
    uint64_t expected_pointer,
    int exact,
    ce_scan_result *result
) {
    const SIZE_T allocation = CE_CHUNK_BYTES + CE_SAMPLE_BYTES - 1u;
    unsigned char *buffer = (unsigned char *)malloc(allocation);
    SIZE_T position = 0;
    if (buffer == NULL) return 0;
    while (position < region_size) {
        SIZE_T starts = region_size - position;
        SIZE_T requested;
        SIZE_T bytes_read = 0;
        SIZE_T offset;
        if (starts > CE_CHUNK_BYTES) starts = CE_CHUNK_BYTES;
        requested = starts + CE_SAMPLE_BYTES - 1u;
        if (requested > region_size - position) requested = region_size - position;
        if (!ReadProcessMemory(
                process, (LPCVOID)(base + position), buffer, requested, &bytes_read)) {
            position += starts;
            continue;
        }
        result->bytes_scanned += bytes_read;
        if (bytes_read >= CE_SAMPLE_BYTES) {
            SIZE_T scan_starts = bytes_read - CE_SAMPLE_BYTES + 1u;
            if (scan_starts > starts) scan_starts = starts;
            for (offset = 0; offset < scan_starts; offset += 4u) {
                ce_candidate candidate;
                if (ce_match_sample(
                        process, buffer + offset, expected_zero, expected_pointer,
                        exact, &candidate)) {
                    ce_record_candidate(result, &candidate);
                }
            }
        }
        position += starts;
    }
    free(buffer);
    return 1;
}

static int ce_scan_process(
    HANDLE process,
    uint64_t expected_zero,
    uint64_t expected_pointer,
    int exact,
    ce_scan_result *result
) {
    SYSTEM_INFO system_info;
    uintptr_t address;
    uintptr_t maximum;
    memset(result, 0, sizeof(*result));
    GetSystemInfo(&system_info);
    address = (uintptr_t)system_info.lpMinimumApplicationAddress;
    maximum = (uintptr_t)system_info.lpMaximumApplicationAddress;
    if (maximum > UINT32_MAX) maximum = UINT32_MAX;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory;
        uintptr_t next;
        if (VirtualQueryEx(process, (LPCVOID)address, &memory, sizeof(memory)) !=
                sizeof(memory)) {
            address += UINT64_C(0x10000);
            continue;
        }
        next = (uintptr_t)memory.BaseAddress + memory.RegionSize;
        if (next <= address) break;
        if (memory.State == MEM_COMMIT && ce_is_readable(memory.Protect) &&
                memory.RegionSize >= CE_SAMPLE_BYTES) {
            ++result->regions;
            if (!ce_scan_region(
                    process, (uintptr_t)memory.BaseAddress, memory.RegionSize,
                    expected_zero, expected_pointer, exact, result)) return 0;
        }
        address = next;
    }
    return 1;
}

static int ce_parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || *text == '\0') return 0;
    parsed = strtoull(text, &end, 16);
    if (end == text || *end != '\0') return 0;
    *value = (uint64_t)parsed;
    return 1;
}

static void ce_print_result(
    DWORD process_id,
    uint64_t expected_zero,
    uint64_t expected_pointer,
    int exact,
    const ce_scan_result *result
) {
    size_t index;
    printf(
        "{\"schema\":1,\"read_only\":true,\"process_id\":%lu,"
        "\"profile\":\"%s\",\"expected_zero_mask\":\"%016" PRIX64 "\","
        "\"expected_pointer_mask\":\"%016" PRIX64 "\",\"regions\":%" PRIu64 ","
        "\"bytes_scanned\":%" PRIu64 ",\"candidate_count\":%" PRIu64 ","
        "\"reported_count\":%zu,\"truncated\":%s,\"candidates\":[",
        (unsigned long)process_id, exact ? "exact" : "stable-required",
        expected_zero, expected_pointer, result->regions, result->bytes_scanned,
        result->candidate_count, result->reported_count,
        result->candidate_count > result->reported_count ? "true" : "false"
    );
    for (index = 0; index < result->reported_count; ++index) {
        const ce_candidate *candidate = &result->reported[index];
        if (index != 0) putchar(',');
        printf(
            "{\"block_fnv1a32\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],"
            "\"zero_mask\":\"%016" PRIX64 "\","
            "\"readable_pointer_mask\":\"%016" PRIX64 "\"}",
            candidate->hashes[0], candidate->hashes[1],
            candidate->hashes[2], candidate->hashes[3],
            candidate->zero_mask, candidate->pointer_mask
        );
    }
    puts("]}");
}

static int ce_self_test(void) {
    unsigned char *sample = (unsigned char *)VirtualAlloc(
        NULL, CE_SAMPLE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );
    unsigned char *pointer_target = (unsigned char *)VirtualAlloc(
        NULL, 4096u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );
    ce_candidate candidate;
    ce_scan_result result;
    unsigned index;
    int matched;
    if (sample == NULL || pointer_target == NULL || (uintptr_t)pointer_target > UINT32_MAX) {
        if (sample != NULL) VirtualFree(sample, 0, MEM_RELEASE);
        if (pointer_target != NULL) VirtualFree(pointer_target, 0, MEM_RELEASE);
        fputs("FAIL: self-test allocation\n", stderr);
        return 2;
    }
    for (index = 0; index < CE_DWORD_COUNT; ++index) {
        uint32_t value = UINT32_C(0x01010101) + index * 2u;
        if ((CE_STABLE_ZERO_MASK & (UINT64_C(1) << index)) != 0) value = 0;
        if ((CE_STABLE_POINTER_MASK & (UINT64_C(1) << index)) != 0) {
            value = (uint32_t)(uintptr_t)pointer_target;
        }
        memcpy(sample + index * 4u, &value, sizeof(value));
    }
    matched = ce_match_sample(
        GetCurrentProcess(), sample, CE_STABLE_ZERO_MASK,
        CE_STABLE_POINTER_MASK, 1, &candidate
    );
    memset(&result, 0, sizeof(result));
    if (matched) {
        matched = ce_scan_region(
            GetCurrentProcess(), (uintptr_t)sample, CE_SAMPLE_BYTES,
            CE_STABLE_ZERO_MASK, CE_STABLE_POINTER_MASK, 1, &result
        ) && result.candidate_count == 1 && result.reported_count == 1;
    }
    VirtualFree(pointer_target, 0, MEM_RELEASE);
    VirtualFree(sample, 0, MEM_RELEASE);
    if (!matched) {
        fputs("FAIL: synthetic Galaxy signature did not survive read-only scan\n", stderr);
        return 2;
    }
    puts("OK: read-only matcher self-test passed");
    return 0;
}

static void ce_usage(const char *program) {
    fprintf(
        stderr,
        "Usage:\n"
        "  %s --self-test\n"
        "  %s --pid PID [--zero-mask HEX] [--pointer-mask HEX] [--exact]\n",
        program, program
    );
}

int main(int argc, char **argv) {
    DWORD process_id = 0;
    uint64_t zero_mask = CE_STABLE_ZERO_MASK;
    uint64_t pointer_mask = CE_STABLE_POINTER_MASK;
    int exact = 0;
    int index;
    HANDLE process;
    ce_scan_result result;
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return ce_self_test();
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--pid") == 0 && index + 1 < argc) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != '\0' || parsed == 0) {
                ce_usage(argv[0]);
                return 2;
            }
            process_id = (DWORD)parsed;
        } else if (strcmp(argv[index], "--zero-mask") == 0 && index + 1 < argc) {
            if (!ce_parse_u64(argv[++index], &zero_mask)) return 2;
        } else if (strcmp(argv[index], "--pointer-mask") == 0 && index + 1 < argc) {
            if (!ce_parse_u64(argv[++index], &pointer_mask)) return 2;
        } else if (strcmp(argv[index], "--exact") == 0) {
            exact = 1;
        } else {
            ce_usage(argv[0]);
            return 2;
        }
    }
    if (process_id == 0 || (zero_mask & pointer_mask) != 0) {
        ce_usage(argv[0]);
        return 2;
    }
    process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process_id);
    if (process == NULL) {
        fprintf(stderr, "FAIL: OpenProcess read-only error=%lu\n", (unsigned long)GetLastError());
        return 3;
    }
    if (!ce_scan_process(process, zero_mask, pointer_mask, exact, &result)) {
        CloseHandle(process);
        fputs("FAIL: scan allocation\n", stderr);
        return 3;
    }
    CloseHandle(process);
    ce_print_result(process_id, zero_mask, pointer_mask, exact, &result);
    return result.candidate_count == 1 ? 0 : 4;
}
