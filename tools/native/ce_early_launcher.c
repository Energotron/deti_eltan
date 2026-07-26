#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

enum {
    CE_PATH_CAPACITY = 32768,
    CE_INJECT_TIMEOUT_MS = 15000
};

static int ce_file_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int ce_parent_directory(wchar_t *path) {
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == NULL) return 0;
    if (slash == path + 2 && path[1] == L':') {
        slash[1] = L'\0';
    } else {
        *slash = L'\0';
    }
    return 1;
}

static int ce_join_path(
        wchar_t *output, size_t output_capacity,
        const wchar_t *left, const wchar_t *right) {
    int written = _snwprintf(
        output, output_capacity, L"%ls%ls%ls",
        left,
        left[0] != L'\0' && left[wcslen(left) - 1] == L'\\' ? L"" : L"\\",
        right);
    if (written < 0 || (size_t)written >= output_capacity) {
        if (output_capacity != 0) output[0] = L'\0';
        return 0;
    }
    return 1;
}

static int ce_find_game_root(
        const wchar_t *launcher_directory,
        wchar_t *game_root, size_t game_root_capacity) {
    wchar_t candidate[CE_PATH_CAPACITY];
    wchar_t executable[CE_PATH_CAPACITY];
    unsigned int level;

    if (wcslen(launcher_directory) >= game_root_capacity) return 0;
    wcscpy(candidate, launcher_directory);
    for (level = 0; level != 5; ++level) {
        if (!ce_join_path(
                executable, CE_PATH_CAPACITY, candidate, L"Rangers.exe")) return 0;
        if (ce_file_exists(executable)) {
            wcscpy(game_root, candidate);
            return 1;
        }
        if (!ce_parent_directory(candidate)) break;
    }
    return 0;
}

static int ce_find_adapter(
        const wchar_t *launcher_directory,
        const wchar_t *game_root,
        wchar_t *adapter_path, size_t adapter_path_capacity) {
    wchar_t candidate[CE_PATH_CAPACITY];
    static const wchar_t *relative_candidates[] = {
        L"DATA\\CESecondMapAdapter.dll",
        L"Mods\\ChildrenOfEltanSmoke\\DATA\\CESecondMapAdapter.dll",
        L"Mods\\ChildrenOfEltan\\DATA\\CESecondMapAdapter.dll"
    };
    size_t index;

    if (ce_join_path(
            candidate, CE_PATH_CAPACITY, launcher_directory,
            relative_candidates[0]) &&
        ce_file_exists(candidate)) {
        if (wcslen(candidate) >= adapter_path_capacity) return 0;
        wcscpy(adapter_path, candidate);
        return 1;
    }
    for (index = 1;
            index != sizeof(relative_candidates) / sizeof(relative_candidates[0]);
            ++index) {
        if (ce_join_path(
                candidate, CE_PATH_CAPACITY, game_root,
                relative_candidates[index]) &&
            ce_file_exists(candidate)) {
            if (wcslen(candidate) >= adapter_path_capacity) return 0;
            wcscpy(adapter_path, candidate);
            return 1;
        }
    }
    return 0;
}

static int ce_rangers_is_running(void) {
    HANDLE snapshot;
    PROCESSENTRY32W process;
    int found = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&process, sizeof(process));
    process.dwSize = sizeof(process);
    if (Process32FirstW(snapshot, &process)) {
        do {
            if (_wcsicmp(process.szExeFile, L"Rangers.exe") == 0) {
                found = 1;
                break;
            }
        } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    return found;
}

static uintptr_t ce_remote_module_base(DWORD process_id, const wchar_t *module_name) {
    HANDLE snapshot;
    MODULEENTRY32W module;
    uintptr_t result = 0;

    snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&module, sizeof(module));
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, module_name) == 0) {
                result = (uintptr_t)module.modBaseAddr;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return result;
}

static int ce_inject_adapter(
        const PROCESS_INFORMATION *process_info,
        const wchar_t *adapter_path,
        wchar_t *error, size_t error_capacity) {
    HMODULE local_kernel32;
    FARPROC local_load_library;
    uintptr_t local_kernel32_base;
    uintptr_t remote_kernel32_base;
    LPTHREAD_START_ROUTINE remote_load_library;
    SIZE_T path_bytes;
    void *remote_path;
    HANDLE remote_thread;
    DWORD wait_result;
    DWORD load_result = 0;
    unsigned int module_attempt;

    local_kernel32 = GetModuleHandleW(L"kernel32.dll");
    local_load_library = local_kernel32 != NULL
        ? GetProcAddress(local_kernel32, "LoadLibraryW") : NULL;
    local_kernel32_base = (uintptr_t)local_kernel32;
    remote_kernel32_base = 0;
    /* CreateProcess returns before the child has necessarily finished its
       user-mode loader initialization.  In particular, a process created
       suspended may expose only ntdll and produce ERROR_PARTIAL_COPY (299)
       when Toolhelp asks for kernel32.  The launcher therefore starts the
       game normally and polls for kernel32 for at most five seconds.  The
       main menu takes much longer than this to become interactive, so the
       adapter's new-game detour is still installed before the player can
       ask the engine to build a party. */
    for (module_attempt = 0; module_attempt != 500; ++module_attempt) {
        remote_kernel32_base = ce_remote_module_base(
            process_info->dwProcessId, L"kernel32.dll");
        if (remote_kernel32_base != 0) break;
        if (WaitForSingleObject(process_info->hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(10);
    }
    if (local_load_library == NULL || remote_kernel32_base == 0) {
        _snwprintf(error, error_capacity,
            L"Не удалось найти LoadLibraryW в процессе игры (ошибка %lu).",
            (unsigned long)GetLastError());
        return 0;
    }
    remote_load_library = (LPTHREAD_START_ROUTINE)(
        remote_kernel32_base + ((uintptr_t)local_load_library - local_kernel32_base));
    path_bytes = (wcslen(adapter_path) + 1) * sizeof(wchar_t);
    remote_path = VirtualAllocEx(
        process_info->hProcess, NULL, path_bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remote_path == NULL) {
        _snwprintf(error, error_capacity,
            L"VirtualAllocEx завершился ошибкой %lu.",
            (unsigned long)GetLastError());
        return 0;
    }
    if (!WriteProcessMemory(
            process_info->hProcess, remote_path, adapter_path,
            path_bytes, NULL)) {
        _snwprintf(error, error_capacity,
            L"WriteProcessMemory завершился ошибкой %lu.",
            (unsigned long)GetLastError());
        VirtualFreeEx(process_info->hProcess, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    remote_thread = CreateRemoteThread(
        process_info->hProcess, NULL, 0,
        remote_load_library, remote_path, 0, NULL);
    if (remote_thread == NULL) {
        _snwprintf(error, error_capacity,
            L"CreateRemoteThread завершился ошибкой %lu.",
            (unsigned long)GetLastError());
        VirtualFreeEx(process_info->hProcess, remote_path, 0, MEM_RELEASE);
        return 0;
    }
    wait_result = WaitForSingleObject(remote_thread, CE_INJECT_TIMEOUT_MS);
    if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeThread(remote_thread, &load_result);
    }
    CloseHandle(remote_thread);
    VirtualFreeEx(process_info->hProcess, remote_path, 0, MEM_RELEASE);
    if (wait_result != WAIT_OBJECT_0 || load_result == 0) {
        _snwprintf(error, error_capacity,
            L"Игра не загрузила CESecondMapAdapter.dll (wait=%lu, result=0x%08lx).",
            (unsigned long)wait_result, (unsigned long)load_result);
        return 0;
    }
    return 1;
}

static void ce_show_error(const wchar_t *message) {
    MessageBoxW(
        NULL, message, L"Дети Эльтан — ранний запуск",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int WINAPI wWinMain(
        HINSTANCE instance, HINSTANCE previous_instance,
        wchar_t *command_line, int show_command) {
    wchar_t launcher_path[CE_PATH_CAPACITY];
    wchar_t launcher_directory[CE_PATH_CAPACITY];
    wchar_t game_root[CE_PATH_CAPACITY];
    wchar_t game_executable[CE_PATH_CAPACITY];
    wchar_t adapter_path[CE_PATH_CAPACITY];
    wchar_t game_command_line[CE_PATH_CAPACITY];
    wchar_t error[1024];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD launcher_length;
    int dry_run;

    (void)instance;
    (void)previous_instance;
    (void)show_command;
    launcher_length = GetModuleFileNameW(
        NULL, launcher_path, CE_PATH_CAPACITY);
    if (launcher_length == 0 || launcher_length >= CE_PATH_CAPACITY) {
        ce_show_error(L"Не удалось определить путь к лаунчеру.");
        return 1;
    }
    wcscpy(launcher_directory, launcher_path);
    if (!ce_parent_directory(launcher_directory) ||
        !ce_find_game_root(
            launcher_directory, game_root, CE_PATH_CAPACITY) ||
        !ce_join_path(
            game_executable, CE_PATH_CAPACITY, game_root, L"Rangers.exe")) {
        ce_show_error(
            L"Rangers.exe не найден. Помести лаунчер в каталог мода внутри папки игры.");
        return 2;
    }
    if (!ce_find_adapter(
            launcher_directory, game_root,
            adapter_path, CE_PATH_CAPACITY)) {
        ce_show_error(
            L"CESecondMapAdapter.dll не найден рядом с установленным модом.");
        return 3;
    }
    dry_run = command_line != NULL &&
        wcsstr(command_line, L"--dry-run") != NULL;
    if (dry_run) {
        _snwprintf(
            error, sizeof(error) / sizeof(error[0]),
            L"Проверка пройдена.\n\nИгра:\n%ls\n\nАдаптер:\n%ls",
            game_executable, adapter_path);
        MessageBoxW(
            NULL, error, L"Дети Эльтан — ранний запуск",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }
    if (ce_rangers_is_running()) {
        ce_show_error(
            L"Rangers.exe уже запущен. Закрой игру и запусти её этим лаунчером.");
        return 4;
    }
    if (_snwprintf(
            game_command_line,
            sizeof(game_command_line) / sizeof(game_command_line[0]),
            L"\"%ls\"", game_executable) < 0) {
        ce_show_error(L"Командная строка игры слишком длинная.");
        return 5;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(
            game_executable, game_command_line,
            NULL, NULL, FALSE, 0,
            NULL, game_root, &startup, &process)) {
        _snwprintf(error, sizeof(error) / sizeof(error[0]),
            L"Не удалось запустить Rangers.exe (ошибка %lu).",
            (unsigned long)GetLastError());
        ce_show_error(error);
        return 6;
    }
    if (!ce_inject_adapter(
            &process, adapter_path,
            error, sizeof(error) / sizeof(error[0]))) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        ce_show_error(error);
        return 7;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
