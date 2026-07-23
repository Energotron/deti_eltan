#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* One-shot live investigation tool, same methodology that found the
   sector-name "row" objects: needle-search memory for a KNOWN string
   (an item name we know is in the player's cargo right now), find who
   references it, then dump a window around each referencing object so
   the item's real field layout / array stride can be read by eye --
   instead of blindly guessing an array-of-pointers shape (which found
   nothing for the ship's item list). */

static int read_mem(HANDLE process, uint32_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, out, size, &got) && got == size;
}

static int read_u32(HANDLE process, uint32_t address, uint32_t *out) {
    return read_mem(process, address, out, sizeof(*out));
}

static int readable_protect(DWORD protect) {
    DWORD base = protect & 0xffu;
    return (protect & PAGE_GUARD) == 0u && base != PAGE_NOACCESS &&
        (base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
         base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
         base == PAGE_EXECUTE_WRITECOPY);
}

static size_t scan_bytes(HANDLE process, const unsigned char *needle, size_t needle_size,
        uint32_t *hits, size_t hit_capacity) {
    uintptr_t address = 0x00010000u;
    const uintptr_t maximum = 0x7fff0000u;
    size_t hit_count = 0;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory;
        uintptr_t next;
        unsigned char *buffer;
        SIZE_T got = 0, i;
        if (VirtualQueryEx(process, (LPCVOID)address, &memory, sizeof(memory)) != sizeof(memory)) {
            address += 0x10000u;
            continue;
        }
        next = (uintptr_t)memory.BaseAddress + memory.RegionSize;
        if (next > maximum) next = maximum;
        if (next <= address) break;
        if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
                !readable_protect(memory.Protect) ||
                memory.RegionSize < needle_size || memory.RegionSize > 128u * 1024u * 1024u) {
            address = next;
            continue;
        }
        buffer = (unsigned char *)malloc(memory.RegionSize);
        if (buffer != NULL && ReadProcessMemory(process, memory.BaseAddress, buffer,
                memory.RegionSize, &got) && got >= needle_size) {
            i = 0;
            while (i + needle_size <= got) {
                unsigned char *candidate = (unsigned char *)memchr(
                    buffer + i, needle[0], got - i - needle_size + 1u);
                if (candidate == NULL) break;
                i = (SIZE_T)(candidate - buffer);
                if (memcmp(candidate, needle, needle_size) == 0) {
                    if (hit_count < hit_capacity)
                        hits[hit_count] = (uint32_t)((uintptr_t)memory.BaseAddress + i);
                    ++hit_count;
                }
                ++i;
            }
        }
        free(buffer);
        address = next;
    }
    return hit_count;
}

static void dump_window(HANDLE process, uint32_t center, int32_t before, int32_t after) {
    int32_t off;
    for (off = before; off <= after; off += 4) {
        uint32_t address = (uint32_t)((int64_t)center + off);
        uint32_t value = 0u;
        if (read_u32(process, address, &value)) {
            wprintf(L"  %+5d (0x%08x) = 0x%08x (%u)\n", off, address, value, value);
        } else {
            wprintf(L"  %+5d (0x%08x) = <unreadable>\n", off, address);
        }
    }
}

int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    const wchar_t *needle_text;
    size_t needle_len;
    HANDLE process;
    uint32_t string_hits[32];
    size_t string_count;
    size_t i, j;

    if (argc != 3) {
        fwprintf(stderr, L"usage: ce_find_item_object <pid> <needle-text>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    needle_text = argv[2];
    needle_len = wcslen(needle_text) * sizeof(wchar_t);

    process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    string_count = scan_bytes(process, (const unsigned char *)needle_text, needle_len,
        string_hits, 32u);
    wprintf(L"string_count=%zu\n", string_count);
    for (i = 0; i < string_count; ++i) {
        uint32_t refs[16];
        size_t ref_count = scan_bytes(process, (const unsigned char *)&string_hits[i],
            sizeof(string_hits[i]), refs, 16u);
        wprintf(L"--- string hit[%zu] = 0x%08x, ref_count=%zu ---\n", i, string_hits[i], ref_count);
        for (j = 0; j < ref_count; ++j) {
            wprintf(L" ref[%zu] = 0x%08x\n", j, refs[j]);
            dump_window(process, refs[j], -0x20, 0x20);
        }
    }
    CloseHandle(process);
    return 0;
}
