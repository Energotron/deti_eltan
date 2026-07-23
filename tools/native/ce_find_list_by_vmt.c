#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Found VMT 0x008711d0 for a generic list-container class used throughout
   this ship (array_ptr@+4, count@+8, capacity@+0xc, owner-back-ref@+0x14),
   via three sibling list objects at a fixed 0x18-byte stride referenced
   from the ship object. None of those three held the item/cargo count
   (302/303, confirmed separately via CEAdapterWatchShipForChanges) --
   they were 8/31/0. This scans all of memory for MORE instances of the
   SAME VMT and reports any whose count field is near the target, so the
   real cargo list can be found regardless of which object owns it. */

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

int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    uint32_t vmt;
    uint32_t target_count;
    uint32_t tolerance;
    HANDLE process;
    static uint32_t hits[8192];
    size_t hit_count, i;

    if (argc != 5) {
        fwprintf(stderr, L"usage: ce_find_list_by_vmt <pid> <vmt-hex> <target_count> <tolerance>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    vmt = (uint32_t)wcstoul(argv[2], NULL, 16);
    target_count = wcstoul(argv[3], NULL, 10);
    tolerance = wcstoul(argv[4], NULL, 10);

    process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    hit_count = scan_bytes(process, (const unsigned char *)&vmt, sizeof(vmt), hits, 8192u);
    wprintf(L"total vmt hits (as raw bytes, may include non-object matches) = %zu\n", hit_count);
    for (i = 0; i < hit_count; ++i) {
        uint32_t array_ptr = 0u, count = 0u, capacity = 0u, owner = 0u;
        if (!read_u32(process, hits[i] + 4u, &array_ptr)) continue;
        if (!read_u32(process, hits[i] + 8u, &count)) continue;
        if (!read_u32(process, hits[i] + 0xcu, &capacity)) continue;
        if (!read_u32(process, hits[i] + 0x14u, &owner)) continue;
        if (count + tolerance >= target_count && count <= target_count + tolerance) {
            wprintf(L"MATCH object=0x%08x array_ptr=0x%08x count=%u capacity=%u owner=0x%08x\n",
                hits[i], array_ptr, count, capacity, owner);
        }
    }
    wprintf(L"scan complete\n");
    CloseHandle(process);
    return 0;
}
