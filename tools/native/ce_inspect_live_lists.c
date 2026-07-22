#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

static int readable_protect(DWORD protect) {
    DWORD base = protect & 0xffu;
    return (protect & PAGE_GUARD) == 0u && base != PAGE_NOACCESS &&
        (base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
         base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
         base == PAGE_EXECUTE_WRITECOPY);
}

static size_t scan_bytes(HANDLE process, const unsigned char *needle, size_t needle_size,
        uint32_t *hits, size_t hit_capacity) {
    uintptr_t address = 0x01000000u;
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
    return hit_count;
}

static int read_mem(HANDLE process, uint32_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, out, size, &got) && got == size;
}

static int read_u32(HANDLE process, uint32_t address, uint32_t *out) {
    return read_mem(process, address, out, sizeof(*out));
}

static int read_unicode(HANDLE process, uint32_t pointer, wchar_t *out, size_t capacity) {
    uint32_t length = 0;
    size_t i;
    if (pointer < 0x10000u || !read_u32(process, pointer - 4u, &length) ||
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

static int read_ansi(HANDLE process, uint32_t pointer, wchar_t *out, size_t capacity) {
    uint32_t length = 0;
    char bytes[256];
    size_t i;
    int converted;
    if (pointer < 0x10000u || !read_u32(process, pointer - 4u, &length) ||
            length == 0u || length >= sizeof(bytes) || length >= capacity) return 0;
    if (!read_mem(process, pointer, bytes, length)) return 0;
    for (i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)bytes[i];
        if (ch < 0x20u || ch == 0x7fu) return 0;
    }
    converted = MultiByteToWideChar(1251, 0, bytes, (int)length, out, (int)capacity - 1);
    if (converted <= 0) return 0;
    out[converted] = L'\0';
    return 1;
}

static void inspect_object(HANDLE process, uint32_t object, uint32_t index) {
    uint32_t offset;
    wprintf(L"object[%u]=%08x", index, object);
    for (offset = 0; offset < 0x180u; offset += 4u) {
        uint32_t value = 0;
        wchar_t text[128];
        if (read_u32(process, object + offset, &value)) {
            if (read_unicode(process, value, text, sizeof(text) / sizeof(text[0])))
                wprintf(L" +%03x=U\"%ls\"", offset, text);
            else if (read_ansi(process, value, text, sizeof(text) / sizeof(text[0])))
                wprintf(L" +%03x=A\"%ls\"", offset, text);
        }
    }
    wprintf(L"\n");
    if (index == 0u) {
        for (offset = 0; offset < 0x80u; offset += 4u) {
            uint32_t value = 0;
            if (read_u32(process, object + offset, &value))
                wprintf(L"  +%03x=%08x\n", offset, value);
        }
        {
            uint32_t value = 0;
            unsigned char bytes[64];
            if (read_u32(process, object + 0x10u, &value) && value >= 0x10000u &&
                    read_mem(process, value - 16u, bytes, sizeof(bytes))) {
                wprintf(L"  raw(+010 @ %08x - 16):", value);
                for (offset = 0; offset < sizeof(bytes); ++offset)
                    wprintf(L" %02x", (unsigned)bytes[offset]);
                wprintf(L"\n");
            }
        }
    }
}

int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    uint32_t galaxy;
    HANDLE process;
    uint32_t offset;
    setlocale(LC_ALL, ".UTF8");
    if (argc < 3 || argc > 4) {
        fwprintf(stderr, L"usage: ce_inspect_live_lists <pid> <galaxy-hex> [scan-only]\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    galaxy = wcstoul(argv[2], NULL, 16);
    process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    for (offset = 0; argc == 3 && offset < 0x200u; offset += 4u) {
        uint32_t list = 0, array = 0, count = 0, capacity = 0;
        if (!read_u32(process, galaxy + offset, &list) || list < 0x10000u ||
                !read_u32(process, list + 4u, &array) ||
                !read_u32(process, list + 8u, &count) ||
                !read_u32(process, list + 12u, &capacity) || count > 10000u ||
                capacity > 100000u || count > capacity) continue;
        wprintf(L"galaxy+%03x list=%08x array=%08x count=%u capacity=%u\n",
            offset, list, array, count, capacity);
        if (count != 0u && array >= 0x10000u) {
            uint32_t i, limit = count == 73u ? count : (count < 25u ? count : 25u);
            for (i = 0; i < limit; ++i) {
                uint32_t object = 0;
                if (read_u32(process, array + i * 4u, &object) && object >= 0x10000u)
                    inspect_object(process, object, i);
            }
        }
    }
    {
        static const wchar_t needle_text[] = L"Хоот";
        uint32_t string_hits[64], refs[256];
        size_t string_count = scan_bytes(process, (const unsigned char *)needle_text,
            (sizeof(needle_text) - sizeof(wchar_t)), string_hits, 64u);
        size_t i;
        wprintf(L"needle Хоот occurrences=%zu\n", string_count);
        for (i = 0; i < string_count && i < 64u; ++i) {
            uint32_t byte_length = 0;
            size_t ref_count;
            size_t j;
            read_u32(process, string_hits[i] - 4u, &byte_length);
            ref_count = scan_bytes(process, (const unsigned char *)&string_hits[i],
                sizeof(string_hits[i]), refs, 256u);
            wprintf(L"  string=%08x byte_length=%u refs=%zu", string_hits[i],
                byte_length, ref_count);
            for (j = 0; j < ref_count && j < 64u; ++j) wprintf(L" %08x", refs[j]);
            wprintf(L"\n");
            for (j = 0; j < ref_count && j < 8u; ++j) {
                uint32_t base = refs[j] >= 0x40u ? refs[j] - 0x40u : refs[j];
                uint32_t k;
                wprintf(L"    around ref %08x:\n", refs[j]);
                for (k = 0; k < 0x80u; k += 4u) {
                    uint32_t value = 0;
                    wchar_t decoded[128];
                    if (!read_u32(process, base + k, &value)) continue;
                    wprintf(L"      %08x=%08x", base + k, value);
                    if (read_unicode(process, value, decoded,
                            sizeof(decoded) / sizeof(decoded[0])))
                        wprintf(L" U\"%ls\"", decoded);
                    wprintf(L"\n");
                }
            }
        }
    }
    {
        uint32_t sector_vmt = 0x007fa0c4u;
        uint32_t objects[512];
        size_t object_count = scan_bytes(process, (const unsigned char *)&sector_vmt,
            sizeof(sector_vmt), objects, 512u);
        size_t i, named = 0;
        wprintf(L"candidate vmt %08x occurrences=%zu\n", sector_vmt, object_count);
        for (i = 0; i < object_count && i < 512u; ++i) {
            uint32_t name = 0;
            wchar_t decoded[128];
            if (read_u32(process, objects[i] + 0x0cu, &name) &&
                    read_unicode(process, name, decoded,
                        sizeof(decoded) / sizeof(decoded[0]))) {
                wprintf(L"  candidate=%08x name=\"%ls\"\n", objects[i], decoded);
                ++named;
            }
        }
        wprintf(L"candidate named=%zu\n", named);
    }
    {
        uint32_t row_vmt = 0x008273a8u;
        uint32_t rows[2048];
        size_t row_count = scan_bytes(process, (const unsigned char *)&row_vmt,
            sizeof(row_vmt), rows, 2048u);
        size_t i;
        wprintf(L"row vmt %08x occurrences=%zu\n", row_vmt, row_count);
        for (i = 0; i < row_count && i < 2048u; ++i) {
            uint32_t index_text = 0, name_text = 0;
            wchar_t index_decoded[32], name_decoded[128];
            if (read_u32(process, rows[i] + 0x14u, &index_text) &&
                    read_u32(process, rows[i] + 0x18u, &name_text) &&
                    read_unicode(process, index_text, index_decoded,
                        sizeof(index_decoded) / sizeof(index_decoded[0])) &&
                    read_unicode(process, name_text, name_decoded,
                        sizeof(name_decoded) / sizeof(name_decoded[0])))
                wprintf(L"  row=%08x index=\"%ls\" name=\"%ls\"\n",
                    rows[i], index_decoded, name_decoded);
        }
    }
    CloseHandle(process);
    return 0;
}
