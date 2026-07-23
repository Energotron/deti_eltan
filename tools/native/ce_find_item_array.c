#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* One-shot live investigation tool: the per-turn ship-memory watcher in
   the DLL (CEAdapterWatchShipForChanges) found that ship_ptr+0xEC holds
   the ship's item/cargo count (confirmed twice: 302 after an "ARTS" dev
   cheat granted every artifact in the game, then 303 after one more item
   was added) -- but the count field alone does not tell us WHERE the 303
   actual item pointers live. This scans every dword in the ship's 0x400-
   byte window for a value that, when dereferenced AS an array base,
   yields a long run of dwords that all look like valid, distinct,
   in-range heap pointers -- i.e. a real pointer array, not a scalar. */

static int read_mem(HANDLE process, uint32_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, out, size, &got) && got == size;
}

static int read_u32(HANDLE process, uint32_t address, uint32_t *out) {
    return read_mem(process, address, out, sizeof(*out));
}

/* A candidate array element must be a plausible heap pointer: not tiny,
   not obviously a small integer/flag, and itself readable (so it could
   be a real object). */
static int looks_like_pointer(HANDLE process, uint32_t value) {
    uint32_t probe;
    if (value < 0x00010000u) return 0;
    return read_u32(process, value, &probe);
}

int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    uint32_t ship_ptr;
    uint32_t expected_count;
    HANDLE process;
    uint32_t offset;

    if (argc != 4) {
        fwprintf(stderr, L"usage: ce_find_item_array <pid> <ship_ptr-hex> <expected_count>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    ship_ptr = (uint32_t)wcstoul(argv[2], NULL, 16);
    expected_count = wcstoul(argv[3], NULL, 10);

    process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    for (offset = 0u; offset < 0x2000u; offset += 4u) {
        uint32_t candidate = 0u;
        uint32_t run, i;
        uint32_t elements[512];
        if (!read_u32(process, ship_ptr + offset, &candidate)) continue;
        if (candidate < 0x00010000u) continue;
        /* Try both "candidate points straight at element 0" and "candidate
           is a small header object whose own +4 is the real array base"
           (the generic list shape already proven elsewhere in this
           project: VMT-less header with array pointer at +4, count at
           +8). */
        for (run = 0u; run < 2u; ++run) {
            uint32_t base = candidate;
            uint32_t valid_streak = 0u;
            uint32_t max_check;
            if (run == 1u) {
                if (!read_u32(process, candidate + 4u, &base)) continue;
                if (base < 0x00010000u) continue;
            }
            max_check = expected_count + 4u;
            if (max_check > 512u) max_check = 512u;
            for (i = 0u; i < max_check; ++i) {
                uint32_t element;
                if (!read_u32(process, base + i * 4u, &element)) break;
                elements[i] = element;
                if (!looks_like_pointer(process, element)) break;
                ++valid_streak;
            }
            if (valid_streak >= 30u) {
                wprintf(L"CANDIDATE ship+0x%x (val=0x%x)%hs base=0x%x valid_streak=%u "
                    L"first=0x%x second=0x%x\n",
                    offset, candidate, run == 1u ? " [via +4 header]" : "",
                    base, valid_streak,
                    valid_streak > 0u ? elements[0] : 0u,
                    valid_streak > 1u ? elements[1] : 0u);
            }
        }
    }
    wprintf(L"scan complete\n");
    CloseHandle(process);
    return 0;
}
