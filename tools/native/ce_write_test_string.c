#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* One-shot external write test: overwrite an already-known Unicode string's
   characters in place (same character count as the original -- no
   reallocation, no length-prefix change, so this cannot corrupt the
   surrounding heap block). Used to test whether a specific live string
   address actually drives on-screen text, entirely from OUTSIDE the game
   process via WriteProcessMemory -- the game keeps running normally either
   way, this tool cannot crash it. */
int wmain(int argc, wchar_t **argv) {
    DWORD pid;
    uint32_t address;
    HANDLE process;
    const wchar_t *replacement;
    size_t length;
    SIZE_T written = 0;

    if (argc != 4) {
        fwprintf(stderr, L"usage: ce_write_test_string <pid> <address-hex> <replacement-text>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 0);
    address = (uint32_t)wcstoul(argv[2], NULL, 16);
    replacement = argv[3];
    length = wcslen(replacement);

    process = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (process == NULL) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    {
        uint32_t old_byte_length = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, (LPCVOID)(uintptr_t)(address - 4u),
                &old_byte_length, sizeof(old_byte_length), &got) || got != sizeof(old_byte_length)) {
            fwprintf(stderr, L"failed to read existing length prefix\n");
            CloseHandle(process);
            return 1;
        }
        if (old_byte_length != length * sizeof(wchar_t)) {
            fwprintf(stderr, L"refusing: existing string is %u bytes, replacement is %zu bytes "
                L"(must match exactly for a safe in-place overwrite)\n",
                old_byte_length, length * sizeof(wchar_t));
            CloseHandle(process);
            return 1;
        }
    }

    if (!WriteProcessMemory(process, (LPVOID)(uintptr_t)address, replacement,
            length * sizeof(wchar_t), &written) || written != length * sizeof(wchar_t)) {
        fwprintf(stderr, L"WriteProcessMemory failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    wprintf(L"OK: wrote %zu chars at %08x\n", length, address);
    CloseHandle(process);
    return 0;
}
