#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int read_mem(HANDLE process, uint32_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, out, size, &got) && got == size;
}

static int read_unicode(HANDLE process, uint32_t pointer, wchar_t *out, size_t capacity) {
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

int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    uint32_t start, end, offset;
    HANDLE process;

    if (argc != 4) {
        fwprintf(stderr, L"usage: ce_dump_range <pid> <start-hex> <end-hex>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    start = (uint32_t)wcstoul(argv[2], NULL, 16);
    end = (uint32_t)wcstoul(argv[3], NULL, 16);

    process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    for (offset = start; offset < end; offset += 4u) {
        uint32_t value = 0;
        wchar_t text[128];
        if (!read_mem(process, offset, &value, sizeof(value))) {
            wprintf(L"%08x=<unreadable>\n", offset);
            continue;
        }
        wprintf(L"%08x=%08x", offset, value);
        if (read_unicode(process, value, text, sizeof(text) / sizeof(text[0])))
            wprintf(L" U\"%ls\"", text);
        wprintf(L"\n");
    }
    CloseHandle(process);
    return 0;
}
