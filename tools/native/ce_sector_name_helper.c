#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* External helper process for live, reversible sector renaming.
   Never runs inside the game's own threads -- it only ever reads/writes
   the target process's memory from OUTSIDE via ReadProcessMemory /
   WriteProcessMemory, so it cannot block or crash Rangers.exe the way an
   in-process background-thread memory scan did earlier this session
   (ce_find_sector_name_references, removed after it correlated with a
   crash). Vanilla Constellations.Name values are fixed constants (not
   per-seed generated), so nothing needs to be "captured" across runs --
   this tool is fully stateless: it always knows both the real vanilla
   name and this mod's Second Home name for every sector index, and just
   picks which one to write based on the requested arm. */

static const wchar_t *const VANILLA_NAMES[21] = {
    NULL, /* index 0 unused, sectors are 1-based */
    L"Гурт", L"Хиша", L"Карагон", L"Зондур", L"Атланта", L"Таолис",
    L"Кайо", L"Оника", L"Хоот", L"Фаави", L"Мергац", L"Айла", L"Перец",
    L"Эльта", L"Валгир", L"Тамуто", L"Нароби", L"Денвер", L"Реггет",
    L"Дицея"
};

/* First version of this pool was uniform two-word "Adjective + Noun"
   (20/20 entries) -- the exact same mistake already fixed once this
   session for stars/planets/stations. The decompiled Constellations.Name
   table above is 100% single words, zero exceptions (unlike Star/Planet/
   RuinName, which mix in some two-word entries) -- rebuilt to match that
   exactly: single invented/real Russian nouns, no compounds. */
static const wchar_t *const SECOND_HOME_NAMES[] = {
    L"Порубежье", L"Приграничье", L"Пустошь", L"Окоём", L"Первопуток",
    L"Взгорье", L"Низина", L"Прогалина", L"Урочище", L"Чащоба",
    L"Раздолье", L"Застенок", L"Заслон", L"Отрог", L"Кряж",
    L"Даль", L"Простор", L"Ширь", L"Захолустье", L"Новоземье"
};
static const size_t SECOND_HOME_NAME_COUNT =
    sizeof(SECOND_HOME_NAMES) / sizeof(SECOND_HOME_NAMES[0]);

static int read_mem(HANDLE process, uint32_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, out, size, &got) && got == size;
}

static int write_mem(HANDLE process, uint32_t address, const void *in, size_t size) {
    SIZE_T wrote = 0;
    return WriteProcessMemory(process, (LPVOID)(uintptr_t)address, in, size, &wrote) && wrote == size;
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

/* Bounded content scan: only committed, private, readable pages, same
   guard rails as ce_inspect_live_lists.c's proven scan_bytes(). One pass,
   no nested reference-chasing beyond the single explicit call below. */
static size_t scan_bytes(HANDLE process, const unsigned char *needle, size_t needle_size,
        uint32_t *hits, size_t hit_capacity) {
    uintptr_t address = 0x00010000u;
    const uintptr_t maximum = 0x40000000u;
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
    /* Confirmed live (Windows Event Log, Application Error 0xc0000005 at
       this module's own RVA 0x159b, mapped via llvm-addr2line straight to
       find_anchor_via_index's `string_hits[i]` read): every caller treats
       this return value as a safe bound for indexing its own fixed-size
       `hits` array, but hit_count was incremented past hit_capacity
       whenever more matches existed than the array could hold, so the
       caller's own `for (i = 0; i < string_count; ++i)` loop read past the
       end of a 16- or 64-element stack array. Clamping the RETURN value
       (not just the write above) is what actually fixes it -- the write
       guard alone only stops corrupting `hits[]` itself, not the caller
       trusting an oversized count afterward. Second Home doubling the
       amount of scannable process memory made hitting >16 matches for a
       short 2-wchar needle ("20") a lot more likely than it was with a
       single galaxy, which is presumably why this was never seen before
       this session. */
    return hit_count > hit_capacity ? hit_capacity : hit_count;
}

/* pointer must already BE a string's own address (e.g. from scan_bytes,
   or already dereferenced) -- see read_unicode_field for the field case. */
static int read_unicode_from_pointer(HANDLE process, uint32_t pointer,
        wchar_t *out, size_t capacity) {
    uint32_t length = 0;
    size_t i;
    if (pointer < 0x10000u || !read_mem(process, pointer - 4u, &length, sizeof(length)) ||
            length == 0u || (length & 1u) != 0u || length / 2u >= capacity) return 0;
    length /= 2u;
    if (!read_mem(process, pointer, out, length * sizeof(wchar_t))) return 0;
    out[length] = L'\0';
    for (i = 0; i < length; ++i) {
        wchar_t ch = out[i];
        if (ch < 0x20 || (ch > 0x7e && (ch < 0x400 || ch > 0x52f))) return 0;
    }
    return 1;
}

/* field_address is the ADDRESS OF A FIELD THAT HOLDS a string pointer
   (e.g. row+0x14, row+0x18) -- dereferences the field first, then decodes
   what it points to. This is the one to use for every row field; using
   read_unicode_from_pointer directly on a field address (instead of what
   it points to) was the exact bug that made every match fail at first. */
static int read_unicode_field(HANDLE process, uint32_t field_address,
        wchar_t *out, size_t capacity) {
    uint32_t pointer = 0;
    if (!read_u32(process, field_address, &pointer)) return 0;
    return read_unicode_from_pointer(process, pointer, out, capacity);
}

/* Allocates a fresh immortal-style Unicode string (same layout as
   ce_make_immortal_unicode in the DLL: int32(-1) refcount, uint32
   byte-length, wide chars, NUL) but in the REMOTE process's own memory,
   via VirtualAllocEx + WriteProcessMemory -- so the pointer we hand back
   is valid for THAT process to dereference, exactly like a normal Delphi
   string it allocated itself. Returns the data pointer (post-header), or
   0 on failure. */
static uint32_t alloc_remote_string(HANDLE process, const wchar_t *text) {
    size_t length = wcslen(text);
    size_t bytes = 8u + (length + 1u) * sizeof(wchar_t);
    unsigned char *local = (unsigned char *)malloc(bytes);
    LPVOID remote;
    uint32_t result = 0;
    if (local == NULL) return 0;
    *(int32_t *)(void *)local = -1;
    *(uint32_t *)(void *)(local + 4u) = (uint32_t)(length * sizeof(wchar_t));
    memcpy(local + 8u, text, (length + 1u) * sizeof(wchar_t));
    remote = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote != NULL) {
        SIZE_T wrote = 0;
        if (WriteProcessMemory(process, remote, local, bytes, &wrote) && wrote == bytes) {
            result = (uint32_t)((uintptr_t)remote + 8u);
        }
    }
    free(local);
    return result;
}

/* A row is valid iff BOTH its index field (+0x14) and name field (+0x18)
   currently point at decodable Unicode strings -- used both to validate
   the anchor's neighbours while walking the array and to bound how far
   the array extends in each direction. */
static int row_is_valid(HANDLE process, uint32_t row) {
    wchar_t index_text[32], name_text[128];
    return read_unicode_field(process, row + 0x14u, index_text,
               sizeof(index_text) / sizeof(index_text[0])) &&
           read_unicode_field(process, row + 0x18u, name_text,
               sizeof(name_text) / sizeof(name_text[0]));
}

/* Primary anchor path: find the fixed vanilla NAME text ("Дицея", sector
   20's name field) and confirm via its neighbour's index text ("20").
   Only works while that name string is still actually referenced by a
   row -- i.e. only on the FIRST rename of a given process, before any
   "second" rename has overwritten every row's +0x18 with a new pointer. */
static uint32_t find_anchor_via_name(HANDLE process) {
    static const wchar_t needle_text[] = L"Дицея";
    static const wchar_t needle_index[] = L"20";
    uint32_t string_hits[16];
    size_t string_count;
    uint32_t refs[64];
    size_t ref_count;
    size_t i;

    string_count = scan_bytes(process, (const unsigned char *)needle_text,
        sizeof(needle_text) - sizeof(wchar_t), string_hits, 16u);
    for (i = 0; i < string_count; ++i) {
        size_t j;
        ref_count = scan_bytes(process, (const unsigned char *)&string_hits[i],
            sizeof(string_hits[i]), refs, 64u);
        for (j = 0; j < ref_count; ++j) {
            uint32_t candidate_row = refs[j] - 0x18u;
            wchar_t index_text[32];
            if (read_unicode_field(process, candidate_row + 0x14u, index_text,
                        sizeof(index_text) / sizeof(index_text[0])) &&
                    wcscmp(index_text, needle_index) == 0) {
                return candidate_row;
            }
        }
    }
    return 0;
}

/* Fallback anchor path: the +0x14 INDEX field (a fixed numeric string,
   "1".."20") is never written by this tool -- only +0x18 (the display
   name) ever changes -- so it stays a valid, arm-independent anchor even
   after every row has already been renamed to Second Home names and the
   vanilla name text no longer exists anywhere in memory. Exact-decode
   comparison (not just the raw byte match) rules out false hits from the
   short "20" pattern recurring as a substring of an unrelated longer
   number elsewhere in memory. */
static uint32_t find_anchor_via_index(HANDLE process) {
    static const wchar_t needle_index[] = L"20";
    uint32_t string_hits[16];
    size_t string_count;
    uint32_t refs[64];
    size_t ref_count;
    size_t i;

    string_count = scan_bytes(process, (const unsigned char *)needle_index,
        sizeof(needle_index) - sizeof(wchar_t), string_hits, 16u);
    for (i = 0; i < string_count; ++i) {
        wchar_t decoded[32];
        size_t j;
        if (!read_unicode_from_pointer(process, string_hits[i], decoded,
                    sizeof(decoded) / sizeof(decoded[0])) ||
                wcscmp(decoded, needle_index) != 0) {
            continue;
        }
        ref_count = scan_bytes(process, (const unsigned char *)&string_hits[i],
            sizeof(string_hits[i]), refs, 64u);
        for (j = 0; j < ref_count; ++j) {
            uint32_t candidate_row = refs[j] - 0x14u;
            if (row_is_valid(process, candidate_row)) {
                return candidate_row;
            }
        }
    }
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    int want_second_home;
    HANDLE process;
    uint32_t anchor_row = 0;
    uint32_t rows[64];
    size_t row_count = 0;
    size_t i;
    int changed = 0, failed = 0;

    if (argc != 3) {
        fwprintf(stderr, L"usage: ce_sector_name_helper <pid> <arm: old|second>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    want_second_home = (wcscmp(argv[2], L"second") == 0);
    if (!want_second_home && wcscmp(argv[2], L"old") != 0) {
        fwprintf(stderr, L"arm must be 'old' or 'second'\n");
        return 2;
    }

    process = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    /* Try the vanilla-name anchor first (fast, very low false-positive
       risk); if that fails -- e.g. every row has already been renamed to
       "second" in a prior run of this same tool, so "Дицея" no longer
       exists anywhere in memory -- fall back to the index-field anchor,
       which stays valid regardless of which arm's names are currently
       written. */
    anchor_row = find_anchor_via_name(process);
    if (anchor_row == 0u) {
        anchor_row = find_anchor_via_index(process);
    }
    if (anchor_row == 0u) {
        fwprintf(stderr, L"could not confirm the sector-name row for the anchor\n");
        CloseHandle(process);
        return 1;
    }

    /* Step 3: walk the confirmed 0x30-stride array outward in both
       directions from the anchor, collecting every valid row -- no
       assumption about total sector count, self-terminating on the
       first row that fails to decode as a valid (index, name) pair. */
    rows[row_count++] = anchor_row;
    {
        uint32_t probe = anchor_row;
        while (row_count < 64u) {
            probe -= 0x30u;
            if (!row_is_valid(process, probe)) break;
            rows[row_count++] = probe;
        }
    }
    {
        uint32_t probe = anchor_row;
        while (row_count < 64u) {
            probe += 0x30u;
            if (!row_is_valid(process, probe)) break;
            rows[row_count++] = probe;
        }
    }
    wprintf(L"found %zu sector-name rows via anchor %08x\n", row_count, anchor_row);

    /* Step 4: for each row, read its own numeric key and write the
       requested arm's name for that key -- vanilla name and Second Home
       name are both fixed constants, nothing needs to be captured. */
    for (i = 0; i < row_count; ++i) {
        wchar_t index_text[32];
        unsigned long idx;
        const wchar_t *target_name;
        uint32_t new_ptr;
        if (!read_unicode_field(process, rows[i] + 0x14u, index_text,
                sizeof(index_text) / sizeof(index_text[0]))) {
            ++failed;
            continue;
        }
        idx = wcstoul(index_text, NULL, 10);
        if (idx < 1 || idx > 20) {
            ++failed;
            continue;
        }
        if (want_second_home) {
            target_name = SECOND_HOME_NAMES[(idx - 1) % SECOND_HOME_NAME_COUNT];
        } else {
            target_name = VANILLA_NAMES[idx];
        }
        new_ptr = alloc_remote_string(process, target_name);
        if (new_ptr == 0u || !write_mem(process, rows[i] + 0x18u, &new_ptr, sizeof(new_ptr))) {
            ++failed;
            continue;
        }
        ++changed;
    }

    wprintf(L"arm=%ls changed=%d failed=%d\n", want_second_home ? L"second" : L"old", changed, failed);
    CloseHandle(process);
    return failed > 0 ? 1 : 0;
}
