#include "ce_second_map_adapter.h"

#include <windows.h>

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* This adapter is compiled with clang/MinGW and calls directly into the
   game's Delphi/Borland-compiled code via raw inline-asm CALLs. Confirmed
   in practice (CEAdapterCreateSecondDestination raised a real
   EAccessViolation on turn 1 of a brand new game) that a fault deep inside
   such a call can corrupt memory outright rather than raising a script-
   level exception the RScript engine catches gracefully -- most likely
   because an internal Delphi exception tries to unwind through our foreign,
   non-Delphi stack frame. clang for this target does not support __try/
   __except (MSVC-only extension), so recover manually: a vectored
   exception handler plus setjmp/longjmp turns any crash during a guarded
   call into a safe "operation failed" return instead of an application
   crash. Verified against a real EXCEPTION_ACCESS_VIOLATION before wiring
   this into the risky galaxy/Con construction paths. */
static jmp_buf g_ce_recovery_point;
static volatile LONG g_ce_guard_active = 0;
static volatile LONG g_ce_veh_installed = 0;
static volatile LONG g_ce_fault_code = 0;
static volatile LONG g_ce_fault_eip = 0;
static volatile LONG g_ce_fault_access_type = -1;
static volatile LONG g_ce_fault_access_address = 0;
static HINSTANCE g_ce_adapter_instance = NULL;
static int ce_region_has_access(const void *pointer, size_t bytes, int need_write);
#define CE_DELPHI_EXCEPTION_CODE 0xeedfadeu
/* For a software-raised Delphi exception (CE_DELPHI_EXCEPTION_CODE), the
   ExceptionRecord's own ExceptionAddress lands inside ntdll/kernel32 (the
   RaiseException plumbing), not at the actual `raise` call site in
   Rangers.exe -- useless for finding which LoadFromStream stage failed.
   A heuristic stack scan from the fault-time ESP for words that look like
   .text return addresses recovers the real call chain without needing a
   real frame-pointer walk (Delphi/Borland code reliably uses EBP frames,
   so return addresses sit at predictable-ish offsets above ESP). */
#define CE_FAULT_STACK_CANDIDATES 12
static volatile LONG g_ce_fault_stack[CE_FAULT_STACK_CANDIDATES];
static volatile LONG g_ce_fault_stack_count = 0;
static HANDLE g_ce_exception_trace_file = INVALID_HANDLE_VALUE;
static volatile LONG g_ce_exception_trace_busy = 0;
static volatile LONG g_ce_exception_trace_sequence = 0;

#define CE_EXCEPTION_TRACE_STACK_WORDS 256
typedef struct ce_exception_trace_record {
    uint32_t magic;
    uint32_t size;
    uint32_t sequence;
    uint32_t process_id;
    uint32_t thread_id;
    uint32_t tick_count;
    uint32_t code;
    uint32_t exception_address;
    uint32_t eip;
    uint32_t esp;
    uint32_t ebp;
    uint32_t parameter_count;
    uint32_t parameters[15];
    uint32_t stack_count;
    uint32_t stack_words[CE_EXCEPTION_TRACE_STACK_WORDS];
    uint32_t message_length;
    unsigned char message[512];
} ce_exception_trace_record;

/* Optional first-chance exception recorder for failures that the Delphi
   loading form catches and collapses into the uninformative `EAbort Err`.
   The file handle is opened only when trace-game-exceptions.flag exists,
   so release play pays no per-exception I/O cost.  This handler path uses
   one fixed stack record and a pre-opened Win32 handle: no heap allocation,
   Delphi calls or string formatting while the process is unwinding. */
static void ce_trace_first_chance_exception(
    EXCEPTION_POINTERS *info, DWORD code
) {
    ce_exception_trace_record record;
    MEMORY_BASIC_INFORMATION region;
    uintptr_t stack_end;
    uint32_t index;
    DWORD written = 0;
    LONG parameter_count;

    if (g_ce_exception_trace_file == INVALID_HANDLE_VALUE ||
        InterlockedCompareExchange(&g_ce_exception_trace_busy, 1, 0) != 0) {
        return;
    }
    ZeroMemory(&record, sizeof(record));
    record.magic = 0x58454543u; /* "CEEX" */
    record.size = sizeof(record);
    record.sequence = (uint32_t)InterlockedIncrement(&g_ce_exception_trace_sequence);
    record.process_id = GetCurrentProcessId();
    record.thread_id = GetCurrentThreadId();
    record.tick_count = GetTickCount();
    record.code = code;
    record.exception_address =
        (uint32_t)(uintptr_t)info->ExceptionRecord->ExceptionAddress;
    record.eip = (uint32_t)info->ContextRecord->Eip;
    record.esp = (uint32_t)info->ContextRecord->Esp;
    record.ebp = (uint32_t)info->ContextRecord->Ebp;
    parameter_count = (LONG)info->ExceptionRecord->NumberParameters;
    if (parameter_count < 0) parameter_count = 0;
    if (parameter_count > 15) parameter_count = 15;
    record.parameter_count = (uint32_t)parameter_count;
    for (index = 0u; index < record.parameter_count; ++index) {
        record.parameters[index] =
            (uint32_t)info->ExceptionRecord->ExceptionInformation[index];
    }
    /* Delphi's Exception object is the second custom RaiseException
       parameter.  Its first field after the VMT is FMessage (AnsiString in
       this 32-bit build).  Capture it while the object is still alive; the
       process exits immediately after GameLoad catches this exception, so
       post-mortem memory inspection cannot recover the missing package
       entry name. */
    if (code == CE_DELPHI_EXCEPTION_CODE && record.parameter_count >= 2u) {
        uint32_t exception_object = record.parameters[1];
        uint32_t message_ptr = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(exception_object + 4u), 4u, 0)) {
            message_ptr = *(const uint32_t *)(uintptr_t)(exception_object + 4u);
        }
        if (message_ptr >= 4u &&
            ce_region_has_access(
                (const void *)(uintptr_t)(message_ptr - 4u), 4u, 0)) {
            uint32_t message_length =
                *(const uint32_t *)(uintptr_t)(message_ptr - 4u);
            if (message_length > sizeof(record.message)) {
                message_length = sizeof(record.message);
            }
            if (message_length != 0u &&
                ce_region_has_access(
                    (const void *)(uintptr_t)message_ptr, message_length, 0)) {
                record.message_length = message_length;
                memcpy(
                    record.message, (const void *)(uintptr_t)message_ptr,
                    message_length);
            }
        }
    }
    if (VirtualQuery(
            (const void *)(uintptr_t)record.esp, &region, sizeof(region)) ==
            sizeof(region) && region.State == MEM_COMMIT) {
        stack_end = (uintptr_t)region.BaseAddress + region.RegionSize;
        while (record.stack_count < CE_EXCEPTION_TRACE_STACK_WORDS &&
               (uintptr_t)record.esp +
                   (record.stack_count + 1u) * sizeof(uint32_t) <= stack_end) {
            record.stack_words[record.stack_count] =
                *(const uint32_t *)(uintptr_t)(
                    record.esp + record.stack_count * sizeof(uint32_t));
            ++record.stack_count;
        }
    }
    WriteFile(g_ce_exception_trace_file, &record, sizeof(record), &written, NULL);
    InterlockedExchange(&g_ce_exception_trace_busy, 0);
}

/* Delphi/Borland's own `raise` statement (e.g. the bounds-check helper
   inside TGalaxy.LoadFromStream's reader, VA 0x0082ef40, confirmed by
   disassembly) does not fault the CPU -- it calls System.@RaiseExcept
   (VA 0x00404e20), which calls Win32 RaiseException() with this exact
   magic code (confirmed: `push 0xeedfade` immediately before the
   RaiseException call). Our guard originally only caught hardware faults
   (access violation and friends); a plain Delphi exception raised this
   way sailed straight through as EXCEPTION_CONTINUE_SEARCH and crashed
   the whole process outright instead of being recovered, since it tries
   to unwind through our foreign, non-Delphi call frame. Treat it as
   another recoverable condition, same as the hardware faults below. */
static LONG WINAPI ce_veh_handler(EXCEPTION_POINTERS *info) {
    DWORD code;
    code = info->ExceptionRecord->ExceptionCode;
    if (InterlockedCompareExchange(&g_ce_guard_active, 0, 0) == 0) {
        /* This path is reached only when the explicit trace flag exists.
           Record every first-chance code: heap-corruption/fail-fast and
           library-specific software exceptions were invisible when this
           was limited to the small recovery whitelist below. */
        ce_trace_first_chance_exception(info, code);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_STACK_OVERFLOW || code == CE_DELPHI_EXCEPTION_CODE) {
        InterlockedExchange(&g_ce_fault_code, (LONG)code);
        InterlockedExchange(&g_ce_fault_eip, (LONG)(uintptr_t)info->ExceptionRecord->ExceptionAddress);
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            info->ExceptionRecord->NumberParameters >= 2) {
            InterlockedExchange(&g_ce_fault_access_type,
                (LONG)info->ExceptionRecord->ExceptionInformation[0]);
            InterlockedExchange(&g_ce_fault_access_address,
                (LONG)info->ExceptionRecord->ExceptionInformation[1]);
        } else {
            InterlockedExchange(&g_ce_fault_access_type, -1);
            InterlockedExchange(&g_ce_fault_access_address, 0);
        }
        {
            /* .text section: VirtualAddress 0x1000, size 0x473800 (confirmed
               via pefile against this exact installed build) -> VA range
               0x401000-0x874800. An earlier version of this filter used
               0x475000 as the high bound, confusing the RVA span with a VA
               span -- that excluded almost the entire section (including
               LoadFromStream itself at VA 0x83b6cc) from ever matching. */
            const uintptr_t text_low = 0x401000u;
            const uintptr_t text_high = 0x875000u;
            uintptr_t esp = (uintptr_t)info->ContextRecord->Esp;
            MEMORY_BASIC_INFORMATION stack_region;
            LONG found = 0;
            uintptr_t offset;
            if (VirtualQuery((const void *)esp, &stack_region, sizeof(stack_region)) == sizeof(stack_region) &&
                stack_region.State == MEM_COMMIT) {
                uintptr_t region_end = (uintptr_t)stack_region.BaseAddress + stack_region.RegionSize;
                for (offset = 0; esp + offset + sizeof(uintptr_t) <= region_end &&
                     offset < 0x8000u && found < CE_FAULT_STACK_CANDIDATES; offset += 4u) {
                    uintptr_t value = *(const uintptr_t *)(esp + offset);
                    if (value >= text_low && value < text_high) {
                        g_ce_fault_stack[found++] = (LONG)value;
                    }
                }
            }
            g_ce_fault_stack_count = found;
        }
        InterlockedExchange(&g_ce_guard_active, 0);
        longjmp(g_ce_recovery_point, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void ce_ensure_veh_installed(void) {
    if (InterlockedCompareExchange(&g_ce_veh_installed, 1, 0) == 0) {
        AddVectoredExceptionHandler(1, ce_veh_handler);
    }
}

static void ce_write_one_marker(const char *dir, const char *file_name, const char *payload, size_t length) {
    char marker_path[MAX_PATH];
    HANDLE file;
    DWORD written = 0;

    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\%s", dir, file_name) < 0) return;
    file = CreateFileA(marker_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, payload, (DWORD)length, &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void ce_write_text_marker(const char *file_name, const char *payload, size_t length) {
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];

    /* Redundant fixed-path copy: rules out any %TEMP% resolution mismatch
       between the game process and the tooling reading these markers back. */
    ce_write_one_marker("C:\\ce_debug", file_name, payload, length);

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return;
    ce_write_one_marker(marker_dir, file_name, payload, length);
}

static int ce_write_binary_file(
    const char *dir, const char *file_name, const void *payload, uint32_t length
) {
    char marker_path[MAX_PATH];
    HANDLE file;
    DWORD written = 0;

    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\%s", dir, file_name) < 0) return 0;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, length, &written, NULL) ||
        !FlushFileBuffers(file) || written != length) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return 1;
}

static int ce_read_binary_file(const char *path, unsigned char **out_data, uint32_t *out_length) {
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *buffer;
    DWORD read_bytes = 0;

    *out_data = NULL;
    *out_length = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(file);
        return 0;
    }
    buffer = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (buffer == NULL) {
        CloseHandle(file);
        return 0;
    }
    if (!ReadFile(file, buffer, (DWORD)size.QuadPart, &read_bytes, NULL) ||
        read_bytes != (DWORD)size.QuadPart) {
        HeapFree(GetProcessHeap(), 0, buffer);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    *out_data = buffer;
    *out_length = (uint32_t)size.QuadPart;
    return 1;
}

static void ce_write_progress(const char *label) {
    char payload[128];
    int size = snprintf(payload, sizeof(payload), "%s\r\n", label);
    if (size > 0) {
        ce_write_text_marker("con-build-progress.log", payload, (size_t)size);
    }
}

static void ce_write_fault_report(const char *label) {
    char payload[512];
    char stack_text[300];
    size_t stack_used = 0;
    LONG stack_count = InterlockedCompareExchange(&g_ce_fault_stack_count, 0, 0);
    LONG i;
    int size;

    stack_text[0] = '\0';
    for (i = 0; i < stack_count && stack_used + 16 < sizeof(stack_text); ++i) {
        int written = snprintf(stack_text + stack_used, sizeof(stack_text) - stack_used,
            "%s0x%08lx", i == 0 ? "" : ",", (unsigned long)g_ce_fault_stack[i]);
        if (written > 0) stack_used += (size_t)written;
    }
    size = snprintf(payload, sizeof(payload),
        "%s: code=0x%08lx eip=0x%08lx access_type=%ld access_addr=0x%08lx stack=[%s]\r\n",
        label,
        (unsigned long)InterlockedCompareExchange(&g_ce_fault_code, 0, 0),
        (unsigned long)InterlockedCompareExchange(&g_ce_fault_eip, 0, 0),
        (long)InterlockedCompareExchange(&g_ce_fault_access_type, 0, 0),
        (unsigned long)InterlockedCompareExchange(&g_ce_fault_access_address, 0, 0),
        stack_text);
    if (size > 0) {
        ce_write_text_marker("con-build-progress.log", payload, (size_t)size);
    }
}

/* Defined further down, alongside the WH_KEYBOARD_LL hook it installs;
   forward-declared so DllMain can start it immediately on load instead of
   lazily from inside a Turn-script call (see the comment on that thread
   proc for why: spawning it from Turn-reachable code reproduced a real
   TGalaxy.NextDay crash on turn 0 of a brand new game, and moving the
   spawn here -- which fires once at process attach, before any galaxy or
   Turn processing exists -- was the only variant that stopped it). */
static DWORD WINAPI ce_hook_thread_proc(LPVOID unused);
static DWORD WINAPI ce_sector_spawn_thread_proc(LPVOID unused);

/* Unconditional load marker: proves the engine actually mapped this DLL
   into its process, independent of whether any exported function is ever
   invoked from a Turn script. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        g_ce_adapter_instance = instance;
        DisableThreadLibraryCalls(instance);
        ce_write_progress("dllmain-process-attach");
        thread = CreateThread(NULL, 0, ce_hook_thread_proc, NULL, 0, NULL);
        if (thread != NULL) CloseHandle(thread);
        /* Started here, at process attach, for the exact same reason as
           ce_hook_thread_proc above: anything reachable from Turn-code
           appears to run nested inside TGalaxy.NextDay's own call stack,
           and a heavy OS call made from there -- CreateThread previously,
           now confirmed CreateProcess too (see ce_spawn_sector_name_helper
           and ce_sector_spawn_thread_proc) -- can crash the game. This
           thread sleeps and polls a plain flag instead of ever being
           called into from Turn-code, so the actual CreateProcess call
           never happens on a NextDay-reachable stack. */
        thread = CreateThread(NULL, 0, ce_sector_spawn_thread_proc, NULL, 0, NULL);
        if (thread != NULL) CloseHandle(thread);
        /* ce_find_sector_name_references disabled: spawning it here
           correlated with a fresh game crash on new-game creation.
           Moving it to a background thread avoided the specific
           Turn-code-nested-in-NextDay hazard, but the scan itself (up to
           1GB, byte-unaligned, up to 8 nested full-address-space passes)
           is heavy enough that something else about it -- sustained CPU/
           memory-bandwidth contention during process startup, or a
           genuine bug in the scan itself -- needs to be isolated with a
           clean before/after test before trying again, not patched
           blindly a second time. */
    }
    return TRUE;
}

static volatile LONG g_ce_galaxy_ptr = 0;
static volatile LONG g_ce_fingerprint_hash = 0;
static volatile LONG g_ce_fingerprint_bytes = 0;
static volatile LONG g_ce_layout_written = 0;
static volatile LONG g_ce_layout_block_hash[4] = {0, 0, 0, 0};
static volatile LONG g_ce_layout_normalized_block_hash[4] = {0, 0, 0, 0};
static volatile LONG g_ce_layout_zero_mask_low = 0;
static volatile LONG g_ce_layout_zero_mask_high = 0;
static volatile LONG g_ce_layout_pointer_mask_low = 0;
static volatile LONG g_ce_layout_pointer_mask_high = 0;
static volatile LONG g_ce_layout_sample_bytes = 0;
static volatile LONG g_ce_layout_observation_tag = 0;
static volatile LONG g_ce_layout_observation_sequence = 0;
static volatile LONG g_ce_layout_observation_count = 0;
static volatile LONG g_ce_layout_observation_lock = 0;
static volatile LONG g_ce_layout_last_observation_tag = 0;
static volatile LONG g_ce_layout_output_latest = 0;
static volatile LONG g_ce_old_galaxy_ptr = 0;
static volatile LONG g_ce_second_galaxy_ptr = 0;
static volatile LONG g_ce_second_snapshot_loaded = 0;
/* Player().FCurStar is never touched by the raw galaxy-pointer swap --
   confirmed the hard way (path-drawing from the wrong point, camera
   losing the ship model, eventual "find star by id" crashes): the ship
   keeps pointing at a TStar that belongs to whichever galaxy was active
   when it last moved, which stops existing in the other arm's own list
   the instant the pointer swap flips which galaxy is "live". The engine's
   own TransferShip (already used elsewhere in this project to relocate
   NPC ships) is the correct, validated way to move it -- far safer than
   guessing FCurStar's compiled byte offset and poking it directly. This
   plain remembered-pointer pair is just enough bookkeeping for Main.txt's
   OnKey code to save the ship's star before transferring it into the new
   arm, then transfer it back to the exact same star on return. */
static volatile LONG g_ce_remembered_ship_star = 0;
static volatile LONG g_ce_active_arm = 0;
static volatile LONG g_ce_native_switch_lock = 0;
/* Rapid re-triggering of the swap (holding/mashing the OnKey hotkey while
   the star map is open) toggled the galaxy pointer underneath the map's
   own rendering multiple times per second and crashed the game -- the
   pointer swap itself never faulted (checkpoints always completed in
   matching pairs), so the fix is a cooldown, not a lock. Once a swap (in
   either direction) succeeds at turn T, no further swap is accepted until
   CurTurn() != T, forcing at least one day to pass (and the UI to settle)
   before the reverse swap is allowed. */
static volatile LONG g_ce_last_swap_turn = -1;
static volatile LONG g_ce_second_generation_status = 0;
static volatile LONG g_ce_snapshot_lock = 0;
static volatile LONG g_ce_fresh_snapshot_lock = 0;
static volatile LONG g_ce_snapshot_done = 0;
static volatile LONG g_ce_save_hook_installed = 0;
static volatile LONG g_ce_save_hook_armed = 0;
static volatile LONG g_ce_save_hook_expected_galaxy = 0;
static void *g_ce_save_trampoline = NULL;
static volatile LONG g_ce_load_hook_installed = 0;
static volatile LONG g_ce_star_lookup_hook_installed = 0;
static volatile LONG g_ce_star_lookup_recovered_count = 0;
static volatile LONG g_ce_day_process_hook_installed = 0;
static volatile LONG g_ce_nextday_label3_hook_installed = 0;
static volatile LONG g_ce_nextday_label3_recovered_count = 0;
static volatile LONG g_ce_day_counter_hook_installed = 0;
static volatile LONG g_ce_loadgame_diag_installed = 0;
static volatile LONG g_ce_post_nextday_guard_installed = 0;
/* Set while a portal-initiated LoadGame is outstanding; consumed by the
   GameLoad form gate stub. See CE_RVA_LOADGAME_FORM_GATE_CALL. */
static volatile LONG g_ce_portal_load_pending = 0;
/* Traveller's own stats, carried across the transition. Second Home is a
   genuine new game, so it ships with ITS OWN captain -- confirmed in play:
   equipment collected in the home arm is simply absent afterwards, because
   the arriving pilot is that new game's starting character rather than the
   traveller. These live in the DLL precisely because DLL globals survive
   LoadGame, which replaces every engine-side object. Captured in the old
   arm from the artifact's OnUseCode (a safe context, outside the Turn
   stack) and re-applied once the arrival block runs in Second Home. */
#define CE_PLAYER_STASH_SLOTS 32u
static volatile LONG g_ce_player_stash[CE_PLAYER_STASH_SLOTS];
static volatile LONG g_ce_player_stash_ready = 0;
/* The traveller's hold and fittings. Equipment turned out to be describable
   entirely in numbers -- ItemType, ItemSize, ItemLevel and ItemOwner read it,
   CreateEquipment / CreateHull / CreateArt rebuild it -- so no string plumbing
   is needed and the existing integer machinery covers it.

   Four fields described the item; they did not describe where it was. Rebuilt
   gear all landed in the hold while the destination captain's own factory kit
   stayed bolted into the slots, so the traveller arrived with two of
   everything and nothing fitted. The fifth field is the slot ItemIsInUse()
   reports (0 for the hold), which is what lets the arrival side put each
   piece back where it was. The last three carry the condition the item was
   in -- wear, akrin, micromodule -- so a battered upgraded weapon does not
   quietly become a factory-fresh one.

   The ninth field is the hull class (ranger / military / pirate), which is a
   separate axis from the item type: every hull reports ItemType 42, and what
   tells them apart is HullType(). Feeding 42 to CreateHull() where it expects
   that class walked the engine off the end of its hull table and took the
   process with it. */
#define CE_ITEM_STASH_MAX 96u
#define CE_ITEM_STASH_FIELDS 9u
static volatile LONG g_ce_item_stash[CE_ITEM_STASH_MAX][CE_ITEM_STASH_FIELDS];
static volatile LONG g_ce_item_stash_count = 0;
static volatile LONG g_ce_day_counter_recovered_count = 0;
static volatile LONG g_ce_load_transform_armed = 0;
static volatile LONG g_ce_load_transform_seed = 0;
static void *g_ce_load_trampoline = NULL;
static volatile LONG g_ce_live_galaxy_ptr = 0;
static volatile LONG g_ce_live_seed = 0;
static volatile LONG g_ce_live_switch_lock = 0;
static volatile LONG g_ce_portal_status = 0;
static volatile LONG g_ce_portal_hole_id = 0;
static volatile LONG g_ce_portal_galaxy_ptr = 0;
static HHOOK g_ce_keyboard_hook = NULL;
static volatile LONG g_ce_f8_down = 0;
static volatile LONG g_ce_cheat_triggered = 0;
static uint32_t g_ce_cheat_progress = 0u;
/* A complete second galaxy cannot be constructed safely after a game has
   already started: TThreadCreateNewGame.Execute replaces the player, every
   global world table and the current TGalaxy as one indivisible operation.
   It can, however, run a second time while the engine is already inside its
   native new-game worker.  The first complete world is written to a normal
   save, the second complete world is generated and written to another, and
   the first is loaded back before the worker returns.  These fields belong
   to that startup-only transaction; no live TGalaxy pointer is retained. */
static volatile LONG g_ce_dual_newgame_hook_installed = 0;
static volatile LONG g_ce_dual_newgame_running = 0;
static volatile LONG g_ce_dual_newgame_status = 0;
static void *g_ce_dual_newgame_trampoline = NULL;
static void *g_ce_package_file_open_trampoline = NULL;
static volatile LONG g_ce_package_file_open_hook_installed = 0;
static VOID (WINAPI *g_ce_exit_process_original)(UINT) = NULL;
static VOID (WINAPI *g_ce_post_quit_message_original)(int) = NULL;
static volatile LONG g_ce_process_exit_trace_installed = 0;
static void *g_ce_unhandled_exception_trampoline = NULL;
static volatile LONG g_ce_unhandled_exception_hook_installed = 0;
static uint32_t g_ce_first_arm_save_path = 0u;
static uint32_t g_ce_second_home_save_path = 0u;
/* ANSI twins of the two paths above, for CE_RVA_GAME_LOAD_PATH_CELL only --
   that slot is an AnsiString the engine widens itself; see
   ce_make_immortal_ansi's comment. Direct SaveGame/LoadGame calls keep
   using the UTF-16 versions. */
static uint32_t g_ce_first_arm_save_path_ansi = 0u;
static uint32_t g_ce_second_home_save_path_ansi = 0u;

/* Steam build 20648864 / Rangers.exe 2.1.2500.0 only. */
enum {
    CE_RANGERS_TIMESTAMP = 0x68eccc46u,
    CE_RANGERS_IMAGE_SIZE = 0x004d1000u,
    CE_RVA_GALAXY_IMPORT_CELL = 0x0048263cu,
    CE_RVA_TGALAXY_CLASS_CELL = 0x00438d90u,
    CE_RVA_TGALAXY_CONSTRUCTOR = 0x00439198u,
    CE_RVA_TGALAXY_INITIALIZE = 0x0043a034u,
    CE_RVA_TGALAXY_SAVE_TO_STREAM = 0x0043a1a4u,
    CE_RVA_TGALAXY_LOAD_FROM_STREAM = 0x0043b6ccu,
    CE_RVA_TGALAXY_GENERATE_STARS = 0x0044ff74u,
    CE_RVA_BUFFER_CLASS_CELL = 0x0042ea5cu,
    CE_RVA_BUFFER_CONSTRUCTOR = 0x0042eab0u,
    CE_RVA_TOBJECT_FREE = 0x000045acu,
    /* "Error in procedure TGalaxy.NextDay label = " sits in .text right
       before this prologue (VA 0x840f08); self in eax, a single boolean
       in dl (matches ce_call_delphi_method_byte's convention). */
    CE_RVA_TGALAXY_NEXTDAY = 0x00440f08u,
    /* TCon: the class TransferShip's "system" destination check accepts
       (found by disassembling the validator at VA 0x6403d9, which raises
       "TransferShip - invalid destination" unless the target IsA one of
       three classes; this is the first of the three, and the one
       TGalaxy.LoadFromStream constructs into [self+0x2c]). */
    CE_RVA_TCON_CLASS_CELL = 0x00438f6cu,
    CE_RVA_TCON_CONSTRUCTOR = 0x004482f8u,
    CE_RVA_LIST_ADD = 0x000161c0u,
    /* Globals TCon's own constructor (0x8482f8) touches: an optional
       "current context" object at CE_RVA_CON_CONTEXT_CELL (guarded by a
       null check in the engine's own code, so not necessarily a problem),
       and two sub-object class-ref cells it unconditionally constructs
       from without any null check. */
    CE_RVA_CON_CONTEXT_CELL = 0x0048c288u,
    CE_RVA_CON_SUBLIST_CLASS_CELL = 0x00471184u,
    CE_RVA_CON_SUBOBJ_CLASS_CELL = 0x00013f74u,
    /* Generic constructor trampoline (test dl; call [eax-0xc] i.e. virtual
       NewInstance) that TCon's own constructor calls 10 times, 6x for
       CE_RVA_CON_SUBLIST_CLASS_CELL and 4x for CE_RVA_CON_SUBOBJ_CLASS_CELL.
       Traced with tools/cfg_disasm.py: both classes share the same default
       NewInstance (VA 0x404544, itself unremarkable GetMem+InitInstance) --
       no per-class override, so nothing found in static analysis explains
       the repeat crash. Used to test each nested class in isolation. */
    CE_RVA_GENERIC_CTOR_TRAMPOLINE = 0x0000457cu,
    /* Raw record allocator (GetMem + zero-fill, not a Delphi constructor --
       no VMT set up) used by TGalaxy.LoadFromStream's header section to
       build [galaxy+0xc4], a list of 0x78-byte per-race records whose
       count comes from [galaxy+0x5c]. Untested hypothesis: TCon's
       constructor faults because our synthetic galaxy never has this list
       populated, and something in its ~6 nested sub-constructors expects
       to find it there. */
    CE_RVA_RAW_RECORD_ALLOC = 0x000067a0u,
    CE_GALAXY_RACE_LIST_COUNT_OFFSET = 0x5cu,
    CE_GALAXY_RACE_LIST_OFFSET = 0xc4u,
    CE_RACE_RECORD_BYTES = 0x78u,
    /* "find SECTOR by id in [self+0x164]'s list, raise CE_DELPHI_EXCEPTION_CODE
       if absent" (VA 0x00841e60) -- the real, repeatedly-reproduced crash
       ("ThCa label = 1" in the OS error dialog, confirmed by disassembling
       the reported address) that happens after a clean swap back to the
       original galaxy following a real visit to the loaded second one.
       Originally described as a STAR lookup; CEAdapterCompareStarConCounts
       plus independent cross-check against the GalaxyEye project confirmed
       self+0x164 is actually the SECTOR list (self+0x2c is stars) -- see
       that function's own updated comment. This makes the HARDCODED id (20)
       one of its three callers (VA 0x0085176d) resolves make direct sense:
       20 is sector "Дицея", the fixed, already-significant last vanilla
       sector this project has used as a landmark elsewhere -- not an
       arbitrary star id. Whether that caller resolves it against a stale
       CE_RVA_CON_CONTEXT_CELL is still the leading (not yet fully proven)
       theory for WHY the lookup fails after a swap. Rather than keep
       guessing which of the (at least three, likely more) callers holds
       the stale reference, patch the CALL TO THE RAISE ITSELF (not the
       lookup function's own entry) so a failed lookup logs and returns 0
       (the same "not found" every caller already has to tolerate
       structurally) instead of crashing the whole game -- turns an
       unrecoverable hard crash into a diagnosable, harmless miss while the
       real stale-reference source is still being tracked down. */
    CE_RVA_STAR_LOOKUP_RAISE_CALL = 0x00441ed3u,
    /* The `except` handler wrapping every single day-skip's own
       TGalaxy.NextDay call (VA 0x0072f89e) -- see
       CEAdapterInstallDayProcessRecovery's own comment. */
    CE_RVA_DAY_PROCESS_RAISE_CALL = 0x0032f89eu,
    /* A THIRD, distinct raise site (VA 0x00840d94), found by disassembling
       from a different OS crash dialog's own reported address
       ("Error in procedure TGalaxy.NextDay label = 3", confirmed via the
       hardcoded format string at VA 0x00840edc: "Error in procedure
       TGalaxy.NextDay label = " with the numeric label appended). Unlike
       CE_RVA_DAY_PROCESS_RAISE_CALL (the outer wrapper around the call TO
       NextDay), this address sits just BEFORE CE_RVA_TGALAXY_NEXTDAY's own
       entry (0x00840f08) in memory, and is reached via a jump rather than
       linear fall-through from the numbered try/except scaffolding right
       above it -- almost certainly one of several internal, individually
       numbered integrity checks the original Pascal source performs while
       preparing/finishing a day tick (this same hardcoded message string,
       with a different label number, is reused at multiple such checks).
       Same fix as the other two: redirect this specific
       `call System.@RaiseExcept` to a log-and-fall-through stub so a
       failed check degrades to "nothing happened here" instead of killing
       the process -- the very next instruction after this call (VA
       0x00840d99) already falls into the function's normal epilogue. */
    CE_RVA_NEXTDAY_LABEL3_RAISE_CALL = 0x00440d94u,
    /* A FOURTH, distinct fault site -- not a `call System.@RaiseExcept` at
       all, unlike the three above, so it needed its own disassembly pass
       (tools/disasm_rangers.py) rather than reuse of ce_install_raise_
       recovery_patch. Confirmed live: EAccessViolation reading address
       0x4c, reported at VA 0x0072fb43, inside the SAME day-process wrapper
       function as CE_RVA_DAY_PROCESS_RAISE_CALL (just further along, past
       its own epilogue jump target). The faulting sequence, at VA
       0x0072fb3c: `mov eax,[0x88263c]` (CE_RVA_GALAXY_IMPORT_CELL itself --
       the SAME two-level "current galaxy pointer" cell ce_resolve_engine_
       galaxy reads); `mov eax,[eax]`; `cmp dword ptr [eax+0x4c],0x12c`;
       `jge 0x0072fb82`. The crash means the engine's own idea of the
       current galaxy reads as NULL at this exact point -- confirmed to
       still happen even with this project's own dual-galaxy pointer swap
       otherwise working normally (the swap itself never writes 0 to that
       cell), so the null window's true origin is still unidentified;
       root-caused enough to patch safely, not enough to explain WHY yet.
       0x0072fb82 is not an invented landing spot -- it is the SAME target
       the original `jge` already jumps to whenever the (otherwise valid)
       counter read here is >= 0x12c, i.e. a code path the engine already
       runs under completely ordinary conditions, so treating "galaxy
       unexpectedly null" the same as "counter already high enough" only
       ever skips whatever this block does (never crashes), and changes
       nothing for the vastly more common case where eax is valid -- see
       CEAdapterInstallDayCounterGuard's own comment for the patch shape. */
    CE_RVA_DAY_COUNTER_CHECK = 0x0032fb3cu,
    CE_RVA_DAY_COUNTER_CONTINUE = 0x0032fb4cu,
    CE_RVA_DAY_COUNTER_SKIP = 0x0032fb82u,
    /* THE ENGINE'S OWN COMPLETE NEW-GALAXY BUILDER.
       Found by scanning the whole .text for E8-rel32 calls to
       CE_RVA_TGALAXY_GENERATE_STARS: there is exactly ONE, at VA
       0x005d4378, inside a single ~8KB function entered at VA 0x005d36d8
       (1701 reachable instructions, ~200 engine calls -- GenerateStars is
       just one of them, the rest being precisely the star-naming, sector
       and economy post-processing this project previously wrote off as
       "needs reverse-engineering, too big"). It never needed
       reverse-engineering: it can simply be CALLED.

       Its Delphi VMT decodes exactly (vmtClassName at VA 0x005d3680 ->
       0x005d36c2 = Pascal shortstring len 20 "TThreadCreateNewGame";
       vmtInstanceSize at 0x005d3684 = 0x4c; vmtParent 0x007f851c;
       vmtDestroy 0x007f8790), which puts the class reference itself at
       0x005d36ac and makes 0x005d36d8 the first user virtual method --
       i.e. TThread.Execute. So a "new game" builds its galaxy on a worker
       thread whose Execute does the whole job and stores the result into
       CE_RVA_GALAXY_IMPORT_CELL (confirmed in its own opening lines:
       `mov dl,1; mov eax,[0x838d90]; call 0x839198` = construct TGalaxy,
       then `mov edx,[0x88263c]; mov [edx],eax` = publish it as current).

       This is what finally makes Second Home a REAL second map: the
       snapshot path this replaces (CreateBareSecondGalaxyForLoad +
       LoadSnapshotIntoSecondGalaxy) loads a copy of the player's OWN
       galaxy, so it is byte-identical by construction -- same stars, same
       planet positions, same ships -- which is exactly what the player
       reported seeing and is not fixable by any amount of crash/animation
       work on top of it. */
    CE_RVA_NEWGAME_THREAD_CLASS = 0x001d36acu,
    CE_RVA_NEWGAME_THREAD_EXECUTE = 0x001d36d8u,
    /* The stock new-game form constructs this class through the inherited
       TThread constructor:
         0x005e0c4c: mov dl,1
         0x005e0c4e: mov eax,[0x005d3660]  ; TThreadCreateNewGame VMT
         0x005e0c53: call 0x007f85e0
       Passing CreateSuspended=true gives us a genuine Delphi TThread
       instance whose base bookkeeping, handle and runtime identity are
       valid.  Its Execute can then be called synchronously through our
       original-code trampoline without ever resuming the spare OS thread. */
    CE_RVA_TTHREAD_CONSTRUCTOR = 0x003f85e0u,
    CE_NEWGAME_THREAD_INSTANCE_SIZE = 0x4cu,
    /* Parent TThread's VMT says its instance size is exactly 0x2c.  Only
       bytes from this offset onward belong to TThreadCreateNewGame and are
       safe to copy from the form-created worker into another real worker.
       Copying bytes 0..0x2b would duplicate the base thread's handles and
       synchronization state, which produced an invalid second save. */
    CE_NEWGAME_THREAD_DERIVED_OFFSET = 0x2cu,
    /* Execute's own first loop reads 8 bytes from Self+0x2d..+0x34 and
       writes them to galaxy+0x50..+0x57 -- the same +0x50 block
       CEAdapterCreateAndEnterSecondGalaxy already copies from the live
       galaxy, i.e. per-game settings (races in play). Copying the current
       galaxy's values back into the synthetic Self reproduces the running
       game's own settings for the generated arm instead of leaving them
       zeroed. */
    CE_NEWGAME_THREAD_SETTINGS_OFFSET = 0x2du,
    CE_NEWGAME_THREAD_SETTINGS_SIZE = 8u,
    /* Two engine-wide globals the new-game builder stomps on entry and
       never puts back, because in a REAL new game it does not have to --
       something later in that flow resets them. Both are pointer-to-value
       cells (the global holds a pointer; the value lives at what it points
       to), matching how the builder itself dereferences them.

       CE_RVA_LOADING_FLAG_CELL: builder does `mov eax,[0x882d5c]; mov
       byte ptr [eax],1` exactly ONCE (VA 0x005d3729) and has no matching
       store of 0 anywhere in its 1701 reachable instructions. The routine
       that normally clears it is the day-process wrapper (`mov byte ptr
       [eax],0` at VA 0x0072f7d3), which also GATES ON IT at VA 0x0072f709
       (`cmp byte ptr [eax],0`). Calling the builder mid-game therefore
       leaves the engine believing a load is permanently in progress.
       CE_RVA_NEWGAME_COUNTER_CELL: zeroed on entry (VA 0x005d3720), read
       again later (VA 0x005d4312, 0x005d4550).

       Restoring the galaxy pointer alone was not enough precisely because
       of these -- hence save/restore around the call rather than after the
       fact. NOT claimed to be exhaustive: the builder makes ~200 engine
       calls and may touch more global state than these two; this is the
       part identified by disassembly, not a proof that nothing else is
       disturbed. */
    CE_RVA_LOADING_FLAG_CELL = 0x00482d5cu,
    CE_RVA_NEWGAME_COUNTER_CELL = 0x00482724u,
    /* Whole-save lifecycle, not just TGalaxy serialization.  Proven from
       all direct callers in Rangers.exe:
         0x006008f8: SaveGame(eax=full filename, edx=display title) -> AL;
         0x00601150: LoadGame(eax=full filename) -> AL.
       The load wrapper is exactly what TThreadGameLoad.Execute calls at
       0x0053c98a.  The save wrapper serializes every global object before
       handing immutable memory buffers to its writer thread. */
    CE_RVA_SAVE_GAME = 0x002008f8u,
    CE_RVA_LOAD_GAME = 0x00201150u,
    /* Every stock caller invokes this immediately before SaveGame.  It
       creates the two 300x225 preview bitmaps exported through the cells at
       0x882318 and 0x883018.  SaveGame merely checks those globals: when
       called directly from TThreadCreateNewGame they are still nil, so it
       emits two zero-length records.  The save menu can read the plain
       heading, but LoadGame rejects the incomplete container.  Blank
       initialized previews are sufficient during native new-game loading;
       the normal UI-only 0x6a9670 render pass is intentionally omitted
       because there is no live game form to capture yet.  SaveGame releases
       both bitmaps through 0x4c5730 after it has handed them to the writer. */
    CE_RVA_PREPARE_SAVE_PREVIEWS = 0x000c5684u,
    /* TGameFolders.GetTurnSavePath(Self, out UnicodeString), called by the
       game's own turn-save path immediately before CE_RVA_SAVE_GAME.  It
       gives us the user's real Documents\SpaceRangersHD\Save directory
       without embedding a machine-specific absolute path. */
    CE_RVA_SAVE_MANAGER_CELL = 0x004826d4u,
    CE_RVA_GET_TURN_SAVE_PATH = 0x0039ffdcu,
    /* TThreadGameLoad.Execute reads:
         edx = [base + CE_RVA_GAME_LOAD_PATH_CELL]; eax = [edx]
       and passes that UnicodeString to LoadGame.  Setting this immortal
       string before RScript calls FormChange('GameLoad') gives us the
       game's own loading form, worker and progress animation. */
    CE_RVA_GAME_LOAD_PATH_CELL = 0x00482880u,
    /* The whole-save wrapper finishes serialization synchronously but
       delegates disk I/O to this persistent writer object.  The next native
       save waits on it using these same two methods; we do the same before
       consuming either sidecar through LoadGame. */
    CE_RVA_SAVE_WRITER_OBJECT = 0x0047b6bcu,
    CE_RVA_THREAD_IS_RUNNING = 0x003f8bb4u,
    CE_RVA_THREAD_WAIT_FOR = 0x003f8bf4u,
    /* Higher-level package file opener used directly by the save loader at
       VA 0x7a069f.  Unlike PACKAGE_ENTRY_LOOKUP, this method returns the
       final yes/no answer after searching every registered package, so its
       trace is small and identifies the exact file that makes LoadGame
       collapse into EAbort("Err"). */
    CE_RVA_PACKAGE_FILE_OPEN = 0x0042e244u,
    /* Log-only diagnostics for the four ways LoadGame (CE_RVA_LOAD_GAME)
       can fail.  Each is a `call System.@RaiseExcept` (0x00404e20) reached
       from its own validation branch, with the message string identified by
       disassembly:
         0x00601256 -> "Cannot open file"            (package open returned 0)
         0x0060129F -> "Bad pre-signature of file"   (header != 'RSG')
         0x00601360 -> "Bad post-signature of file"  (marker != 'EZ')
         0x00601474 -> "Compressed galaxy read fail" (decompressed size
                        mismatch against the length stored in the header)
       At every one of these the frame still belongs to LoadGame, so [ebp-4]
       is its own filename argument -- the stubs read it, which answers "did
       TThreadGameLoad even receive our sidecar path" at the same time as
       "which validation rejected it".  These patches deliberately do NOT
       swallow the exception: each stub logs and then jumps to the real
       RaiseExcept, leaving engine behaviour byte-for-byte unchanged. */
    /* The `call LoadGame` inside TThreadGameLoad.Execute (VA 0x0053c98a;
       the very next instructions store its AL result into the thread
       object's own +0x2c). Patching this specific call -- rather than
       LoadGame's prologue -- lets a stub run the original and record
       whether the portal's load actually succeeded, which is the one thing
       the logs still could not distinguish: the process now leaves with
       exit code 0 (a clean Delphi Halt, not a fault) right after the form
       switch, so "load failed quietly" and "load succeeded but the engine
       then had nowhere to go" look identical from outside. */
    /* Byte inside TGalaxy that LoadGame consults before destroying the
       currently active galaxy: `mov eax,[0x88263c]; mov eax,[eax];
       cmp byte ptr [eax+0x1d1],0; jne <skip Free>` at VA 0x006011E6.
       Setting it keeps a galaxy alive across a load that would otherwise
       free it -- which is exactly what the second arm needs while the
       first arm's save is being loaded back to recover its player. */
    CE_GALAXY_KEEP_ALIVE_FLAG = 0x1d1u,
    /* GFormNext: the engine's "which form runs next" selector, a single
       BYTE reached through the pointer cell at VA 0x00882FD0. Found inside
       the engine's own FormChange script function (VA 0x0065AF5C), which
       ends its name search with exactly:
           mov eax, [0x882fd0]
           mov dl,  [ebp-9]        ; index of the matched form
           mov byte ptr [eax], dl
       The index is the form's slot in the GForm table divided by 4; the
       table itself is built at VA 0x00803A00.. where each form registers
       its name (`mov [eax+<slot>], edx` right before the name is assigned
       via 0x004CFFA8). StarMap sits at slot 0x40, hence index 16.

       This is what the portal transition was missing. A load started from
       the main menu leaves the engine with a form queued; one started
       mid-flight does not, so after LoadGame returned success the message
       loop simply ran out and Delphi halted with ExitCode 0 -- confirmed
       at the exit itself: the recorded ExitProcess return address 0x004053C5
       sits in Halt0, right after `mov eax,[0x878000]` (Delphi's ExitCode
       global) and its push/call, i.e. a normal program end rather than a
       fault, which is why Windows never logged a crash.

       An earlier attempt patched VA 0x0053CF4D instead, on the theory that
       a gate there suppressed the follow-up form. That was wrong twice
       over: the hook never fired once in a live transit, and the function
       it belongs to turned out to select background MUSIC, not forms --
       the names it passes to 0x007F9B0C are 'Base', 'Music', 'Destroyer',
       'Nation.PiratePlanetMain'. That patch is removed. */
    /* Post-NextDay pass (VA 0x00841BE8), called by the day-process wrapper
       right after TGalaxy.NextDay. It takes the galaxy in EAX and dereferences
       [self+0x15C] immediately, with no null check -- so it faults with
       "read of address 0000015C" whenever the current-galaxy cell is empty.
       That cell IS legitimately empty for a moment: LoadGame clears it (VA
       0x00601200) before rebuilding, and a day tick landing in that window
       arrives with nothing to work on. Same family as the guard already
       installed at VA 0x0072FB43. The trampoline returns early on a null
       self and otherwise reproduces the original prologue verbatim. */
    /* The sector count, and the answer to a very long hunt. TGalaxy's own
       constructor hardcodes it: at VA 0x008394CB it does
           mov dword ptr [eax+0x160], 0x14        ; 20 sectors
       and immediately builds the constellation TList into [eax+0x164]:
           mov eax,[0x871184]; call 0x0040457C; mov [edx+0x164], eax
       The sector-creation loop inside GenerateStars (VA 0x0084FFB7) then
       takes its bound straight from [self+0x160] and constructs that many
       TConstellation objects. So the limit is one four-byte immediate.

       This is why no config key for it exists and why adding
       Constellations.Name entries past 20 changed nothing: the names are a
       pool, the count is compiled in. Field semantics confirmed against the
       ranger-tools headers (game-objects/TGalaxy.h: "_160 -- связано с
       количеством секторов", "+0x164 constellations").

       Must be patched BEFORE a galaxy is constructed, i.e. before new-game
       generation -- which is what the early launcher exists for. Patching
       later has no effect on an already-built galaxy. */
    /* TGalaxy.turn, per the ranger-tools header (game-objects/TGalaxy.h:
       "int turn; ///< текущий ход" at +0x4C). A loaded save brings its own
       date with it, so a traveller who leaves on turn 500 lands on whatever
       turn the target world was saved at. Writing this after the load keeps
       both arms on the same date, which is what a corridor between them
       implies. */
    CE_GALAXY_TURN_FIELD = 0x4cu,
    CE_RVA_SECTOR_COUNT_IMMEDIATE = 0x004394d1u,
    CE_RVA_POST_NEXTDAY_PASS = 0x00441be8u,
    CE_RVA_FORM_NEXT_CELL = 0x00482fd0u,
    /* The main loop reads the pending form index out of the cell above and,
       for StarMap and its two neighbours, calls the form object's virtual
       method at +8 -- the enter-the-form path -- then zeroes the byte again.
       These two cells are the bookends: one names the form being entered
       while that call runs, the other the form settled on afterwards. They
       are read only, to say in the log where the engine actually was. */
    CE_RVA_FORM_ENTERING_CELL = 0x00482bf4u,
    CE_RVA_FORM_SETTLED_CELL = 0x00482cf0u,
    CE_FORM_INDEX_STARMAP = 16u,
    /* The GameLoad form's own gate, `call 0x007029A4` at VA 0x0053CF4D,
       immediately followed by `test al,al; je 0x0053CFCF`.  When it answers
       false the form skips the entire block that nominates the follow-up
       form (the branch at VA 0x0053CFA2 hands "StarMap" to 0x007F9B0C), so
       nothing is queued, the message loop simply runs out and Delphi halts
       with exit code 0 -- observed exactly: LoadGame logged ok=1 and the
       process still ended silently, with no fault and no event-log entry.
       Disassembly of the predicate shows it demands [self+0x24] != 0,
       [self+0x1c] == 0, [self+0x20] == 0 and byte [self+0x459] == 0, i.e.
       state that holds for a load started from the main menu but not for
       one started mid-flight from Turn code.  The stub forwards to the
       original and only overrides the answer while this mod's own portal
       load is outstanding, so ordinary menu loads keep the stock gate. */
    CE_RVA_LOADGAME_FORM_GATE_CALL = 0x0013cf4du,
    CE_RVA_LOADGAME_CALL_IN_THREAD = 0x0013c98au,
    CE_RVA_LOADGAME_RAISE_CANNOT_OPEN = 0x00201256u,
    CE_RVA_LOADGAME_RAISE_BAD_PRE_SIG = 0x0020129fu,
    CE_RVA_LOADGAME_RAISE_BAD_POST_SIG = 0x00201360u,
    CE_RVA_LOADGAME_RAISE_GALAXY_READ = 0x00201474u,
    CE_RVA_EXIT_PROCESS_IAT = 0x0048dbd0u,
    CE_RVA_POST_QUIT_MESSAGE_IAT = 0x0048dcfcu,
    CE_RVA_DELPHI_UNHANDLED_EXCEPTION = 0x00004f84u
};

uint32_t CE_CALL CEAdapterAbiVersion(void) {
    return CE_ADAPTER_ABI_VERSION;
}

uint32_t CE_CALL CEAdapterCapabilities(void) {
    return CE_CAP_BIND_GALAXY_POINTER | CE_CAP_SMOKE_MARKER |
        CE_CAP_READONLY_GALAXY_FINGERPRINT |
        CE_CAP_READONLY_GALAXY_LAYOUT_SAMPLE |
        CE_CAP_READONLY_GALAXY_LAYOUT_LATEST |
        CE_CAP_POINTER_NORMALIZED_LAYOUT_HASH |
        CE_CAP_EXPERIMENTAL_ENGINE_GALAXY |
        CE_CAP_NATIVE_GALAXY_SNAPSHOT |
        CE_CAP_SAVE_LIFECYCLE_SNAPSHOT |
        CE_CAP_LOAD_LIFECYCLE_TRANSFORM |
        CE_CAP_LIVE_ARM_SWITCH |
        CE_CAP_MANUAL_PORTAL_TRANSIT |
        CE_CAP_MAP_VISUAL_FIXES;
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
        "{\"abi\":%u,\"galaxy_ptr_nonzero\":true,\"sample_bytes\":%u,"
        "\"fnv1a32\":%u,\"region_size\":%lu,"
        "\"state\":%lu,\"protect\":%lu,\"type\":%lu,\"read_only\":true}\r\n",
        CEAdapterAbiVersion(), byte_count, hash, (unsigned long)memory.RegionSize,
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

static int ce_is_readable_pointer(uint32_t candidate) {
    MEMORY_BASIC_INFORMATION memory;
    uintptr_t address = (uintptr_t)candidate;
    uintptr_t region_start;
    uintptr_t region_end;

    if (candidate < 0x10000u || (candidate & 3u) != 0) {
        return 0;
    }
    if (VirtualQuery((LPCVOID)address, &memory, sizeof(memory)) != sizeof(memory)) {
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    return memory.State == MEM_COMMIT && ce_is_readable_protection(memory.Protect) &&
        address >= region_start && address < region_end;
}

uint32_t CE_CALL CEAdapterSampleGalaxyLayout(uint32_t galaxy_ptr, uint32_t sample_tag) {
    unsigned char sample[256];
    unsigned char normalized[256];
    MEMORY_BASIC_INFORMATION memory;
    SIZE_T bytes_read = 0;
    uintptr_t address = (uintptr_t)galaxy_ptr;
    uintptr_t region_start;
    uintptr_t region_end;
    uint32_t block_hash[4];
    uint32_t normalized_hash[4];
    uint32_t zero_low = 0;
    uint32_t zero_high = 0;
    uint32_t pointer_low = 0;
    uint32_t pointer_high = 0;
    uint32_t index;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[1024];
    int payload_size;
    HANDLE file;
    DWORD written = 0;
    int latest_output;

    if (InterlockedCompareExchange(&g_ce_layout_written, 1, 0) != 0) {
        return 1;
    }
    if (galaxy_ptr == 0 ||
        VirtualQuery((LPCVOID)address, &memory, sizeof(memory)) != sizeof(memory)) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    if (memory.State != MEM_COMMIT || !ce_is_readable_protection(memory.Protect) ||
        address < region_start || address > region_end || sizeof(sample) > region_end - address ||
        !ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, sample,
            sizeof(sample), &bytes_read) || bytes_read != sizeof(sample)) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    memcpy(normalized, sample, sizeof(normalized));
    for (index = 0; index < 64; ++index) {
        uint32_t word;
        uint32_t bit = 1u << (index & 31u);
        memcpy(&word, sample + index * sizeof(word), sizeof(word));
        if (word == 0) {
            if (index < 32) zero_low |= bit; else zero_high |= bit;
        } else if (ce_is_readable_pointer(word)) {
            if (index < 32) pointer_low |= bit; else pointer_high |= bit;
            memset(normalized + index * sizeof(word), 0, sizeof(word));
        }
    }
    for (index = 0; index < 4; ++index) {
        block_hash[index] = ce_fnv1a32(sample + index * 64u, 64u);
        normalized_hash[index] = ce_fnv1a32(normalized + index * 64u, 64u);
        InterlockedExchange(&g_ce_layout_block_hash[index], (LONG)block_hash[index]);
        InterlockedExchange(
            &g_ce_layout_normalized_block_hash[index], (LONG)normalized_hash[index]
        );
    }
    InterlockedExchange(&g_ce_layout_zero_mask_low, (LONG)zero_low);
    InterlockedExchange(&g_ce_layout_zero_mask_high, (LONG)zero_high);
    InterlockedExchange(&g_ce_layout_pointer_mask_low, (LONG)pointer_low);
    InterlockedExchange(&g_ce_layout_pointer_mask_high, (LONG)pointer_high);
    InterlockedExchange(&g_ce_layout_sample_bytes, (LONG)sizeof(sample));

    latest_output = InterlockedCompareExchange(&g_ce_layout_output_latest, 0, 0) != 0;
    if (GetTempPathA(MAX_PATH, temp_path) == 0 ||
        snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0 ||
        (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        snprintf(
            marker_path,
            sizeof(marker_path),
            latest_output ? "%s\\galaxy-layout-latest.json" : "%s\\galaxy-layout-samples.jsonl",
            marker_dir
        ) < 0) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    payload_size = snprintf(
        payload,
        sizeof(payload),
        "{\"abi\":%u,\"process_id\":%lu,\"sample_tag\":%u,"
        "\"observation_tag\":%u,\"sequence\":%u,\"sample_bytes\":256,"
        "\"block_fnv1a32\":[%u,%u,%u,%u],\"zero_mask\":\"%08lX%08lX\","
        "\"pointer_normalized_fnv1a32\":[%u,%u,%u,%u],"
        "\"readable_pointer_mask\":\"%08lX%08lX\",\"raw_values_included\":false,"
        "\"read_only\":true}\r\n",
        CEAdapterAbiVersion(), (unsigned long)GetCurrentProcessId(), sample_tag,
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_tag, 0, 0),
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_sequence, 0, 0),
        block_hash[0], block_hash[1], block_hash[2], block_hash[3],
        (unsigned long)zero_high, (unsigned long)zero_low,
        normalized_hash[0], normalized_hash[1], normalized_hash[2], normalized_hash[3],
        (unsigned long)pointer_high, (unsigned long)pointer_low
    );
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    file = CreateFileA(
        marker_path,
        latest_output ? GENERIC_WRITE : FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        latest_output ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, payload, (DWORD)payload_size, &written, NULL) ||
        !FlushFileBuffers(file)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    CloseHandle(file);
    if (written != (DWORD)payload_size) {
        InterlockedExchange(&g_ce_layout_written, 0);
        return 0;
    }
    return 1;
}

uint32_t CE_CALL CEAdapterObserveGalaxyLayout(
    uint32_t galaxy_ptr, uint32_t sample_tag, uint32_t observation_tag
) {
    uint32_t count;
    uint32_t result;

    if (galaxy_ptr == 0 || sample_tag == 0 ||
        InterlockedCompareExchange(&g_ce_layout_observation_lock, 1, 0) != 0) {
        return 0;
    }
    count = (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_count, 0, 0);
    if (count != 0 &&
        (uint32_t)InterlockedCompareExchange(&g_ce_layout_last_observation_tag, 0, 0) ==
            observation_tag) {
        InterlockedExchange(&g_ce_layout_observation_lock, 0);
        return 1;
    }
    InterlockedExchange(&g_ce_layout_observation_tag, (LONG)observation_tag);
    InterlockedExchange(&g_ce_layout_observation_sequence, (LONG)(count + 1u));
    InterlockedExchange(&g_ce_layout_output_latest, 1);
    InterlockedExchange(&g_ce_layout_written, 0);
    result = CEAdapterSampleGalaxyLayout(galaxy_ptr, sample_tag);
    InterlockedExchange(&g_ce_layout_output_latest, 0);
    if (result != 0) {
        InterlockedExchange(&g_ce_layout_observation_count, (LONG)(count + 1u));
        InterlockedExchange(&g_ce_layout_last_observation_tag, (LONG)observation_tag);
    }
    InterlockedExchange(&g_ce_layout_observation_lock, 0);
    return result;
}

uint32_t CE_CALL CEAdapterGetLayoutObservationCount(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_observation_count, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutBlockHash(uint32_t block_index) {
    if (block_index >= 4) return 0;
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_block_hash[block_index], 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutNormalizedBlockHash(uint32_t block_index) {
    if (block_index >= 4) return 0;
    return (uint32_t)InterlockedCompareExchange(
        &g_ce_layout_normalized_block_hash[block_index], 0, 0
    );
}

uint32_t CE_CALL CEAdapterGetLayoutZeroMaskLow(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_zero_mask_low, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutZeroMaskHigh(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_zero_mask_high, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutReadablePointerMaskLow(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_pointer_mask_low, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutReadablePointerMaskHigh(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_pointer_mask_high, 0, 0);
}

uint32_t CE_CALL CEAdapterGetLayoutSampleBytes(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_layout_sample_bytes, 0, 0);
}

uint32_t CE_CALL CEAdapterSupportsNativeMultiGalaxy(void) {
    return InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0) != 0 ? 1u : 0u;
}

static int ce_region_has_access(const void *pointer, size_t bytes, int need_write) {
    MEMORY_BASIC_INFORMATION memory;
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t region_start;
    uintptr_t region_end;
    DWORD base;

    if (pointer == NULL || bytes == 0 ||
        VirtualQuery(pointer, &memory, sizeof(memory)) != sizeof(memory)) {
        return 0;
    }
    region_start = (uintptr_t)memory.BaseAddress;
    region_end = region_start + memory.RegionSize;
    base = memory.Protect & 0xffu;
    if (memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 ||
        address < region_start || address > region_end || bytes > region_end - address) {
        return 0;
    }
    if (need_write) {
        return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
            base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    }
    return ce_is_readable_protection(memory.Protect);
}

static int ce_resolve_engine_galaxy(
    uint32_t galaxy_ptr,
    uintptr_t *module_base_out,
    uint32_t **galaxy_slot_out,
    uint32_t *class_ref_out
) {
    static const unsigned char constructor_signature[] = {
        0x55, 0x8b, 0xec, 0xb9, 0x0a, 0x00, 0x00, 0x00,
        0x6a, 0x00, 0x6a, 0x00, 0x49, 0x75, 0xf9, 0x51
    };
    static const unsigned char initializer_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xf8, 0x89, 0x45,
        0xfc, 0x8b, 0x45, 0xfc, 0x33, 0xd2, 0x89, 0x50
    };
    static const unsigned char generator_signature[] = {
        0x55, 0x8b, 0xec, 0x81, 0xc4, 0x6c, 0xfe, 0xff,
        0xff, 0x53, 0x56, 0x57, 0x33, 0xc9, 0x89, 0x4d
    };
    HMODULE module = GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS32 *nt;
    uintptr_t base;
    uint32_t **import_cell;
    uint32_t *slot;
    uint32_t class_ref;

    if (module == NULL || galaxy_ptr == 0) return 0;
    base = (uintptr_t)module;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (!ce_region_has_access(dos, sizeof(*dos), 0) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS32 *)(base + (uintptr_t)dos->e_lfanew);
    if (!ce_region_has_access(nt, sizeof(*nt), 0) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != CE_RANGERS_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != CE_RANGERS_IMAGE_SIZE) {
        return 0;
    }
    if (memcmp((const void *)(base + CE_RVA_TGALAXY_CONSTRUCTOR),
            constructor_signature, sizeof(constructor_signature)) != 0 ||
        memcmp((const void *)(base + CE_RVA_TGALAXY_INITIALIZE),
            initializer_signature, sizeof(initializer_signature)) != 0 ||
        memcmp((const void *)(base + CE_RVA_TGALAXY_GENERATE_STARS),
            generator_signature, sizeof(generator_signature)) != 0) {
        return 0;
    }
    import_cell = (uint32_t **)(base + CE_RVA_GALAXY_IMPORT_CELL);
    if (!ce_region_has_access(import_cell, sizeof(*import_cell), 0)) return 0;
    slot = *import_cell;
    if (!ce_region_has_access(slot, sizeof(*slot), 1) || *slot != galaxy_ptr ||
        !ce_region_has_access((const void *)(uintptr_t)galaxy_ptr, 0x1dcu, 0)) {
        return 0;
    }
    class_ref = *(const uint32_t *)(base + CE_RVA_TGALAXY_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)class_ref, sizeof(uint32_t), 0) ||
        *(const uint32_t *)(uintptr_t)galaxy_ptr != class_ref) {
        return 0;
    }
    *module_base_out = base;
    *galaxy_slot_out = slot;
    *class_ref_out = class_ref;
    return 1;
}

static uint32_t ce_call_delphi_constructor(uint32_t class_ref, uintptr_t function_address) {
    uint32_t result;
    __asm__ volatile(
        "movb $1, %%dl\n\t"
        "movl %1, %%eax\n\t"
        "call *%2\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(class_ref), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static void ce_call_delphi_method(uint32_t self, uintptr_t function_address) {
    __asm__ volatile(
        "movl %0, %%eax\n\t"
        "call *%1"
        :
        : "r"(self), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
}

static void ce_call_delphi_method_byte(
    uint32_t self, uint32_t byte_argument, uintptr_t function_address
) {
    __asm__ volatile(
        "movb %b1, %%dl\n\t"
        "movl %0, %%eax\n\t"
        "call *%2"
        :
        : "r"(self), "q"(byte_argument), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
}

static uint32_t ce_call_delphi_method_dword(
    uint32_t self, uint32_t dword_argument, uintptr_t function_address
) {
    uint32_t result;
    __asm__ volatile(
        "movl %1, %%edx\n\t"
        "movl %2, %%eax\n\t"
        "call *%3\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(dword_argument), "r"(self), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static uint32_t ce_call_delphi_eax_edx(
    uint32_t eax_argument, uint32_t edx_argument, uintptr_t function_address
) {
    uint32_t result;
    __asm__ volatile(
        "movl %1, %%edx\n\t"
        "movl %2, %%eax\n\t"
        "call *%3\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(edx_argument), "r"(eax_argument), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static uint32_t ce_call_raw_alloc(uint32_t size, uintptr_t function_address) {
    uint32_t result;
    __asm__ volatile(
        "movl %1, %%eax\n\t"
        "call *%2\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(size), "r"(function_address)
        : "eax", "ecx", "edx", "memory"
    );
    return result;
}

static void ce_write_native_stage(uint32_t stage, uint32_t star_count) {
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[160];
    HANDLE file;
    DWORD written = 0;
    int payload_size;

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\native-second-galaxy.json", marker_dir) < 0) return;
    payload_size = snprintf(payload, sizeof(payload),
        "{\"abi\":%u,\"stage\":%u,\"stars\":%u}\r\n",
        CEAdapterAbiVersion(), stage, star_count);
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) return;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, payload, (DWORD)payload_size, &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

/* Dev-only diagnostic: dumps raw dwords [0x00, 0x200) of galaxy_ptr so two
   snapshots (a known-working real galaxy vs. the synthetic second one) can
   be diffed offline to find which field GalaxyStars() actually reads. Not
   part of the read-only fingerprint capability (that one intentionally
   never emits raw values); this is throwaway spike tooling. */
uint32_t CE_CALL CEAdapterDumpGalaxyWords(uint32_t galaxy_ptr, uint32_t tag) {
    static const uint32_t dump_bytes = 0x200u;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[4096];
    int offset = 0;
    int written_chars;
    uint32_t index;
    HANDLE file;
    DWORD written = 0;

    if (galaxy_ptr == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)galaxy_ptr, dump_bytes, 0)) {
        return 0;
    }
    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\galaxy-dump-%u.json", marker_dir, tag) < 0) {
        return 0;
    }
    written_chars = snprintf(payload, sizeof(payload), "{\"abi\":%u,\"tag\":%u,\"words\":[",
        CEAdapterAbiVersion(), tag);
    if (written_chars <= 0) return 0;
    offset = written_chars;
    for (index = 0; index < dump_bytes / 4u; ++index) {
        uint32_t value = *(const uint32_t *)(uintptr_t)(galaxy_ptr + index * 4u);
        written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset,
            index == 0 ? "%u" : ",%u", value);
        if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
        offset += written_chars;
    }
    written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset, "]}\r\n");
    if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
    offset += written_chars;
    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)offset, &written, NULL) || !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return written == (DWORD)offset ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterProbeEngineGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    return ce_resolve_engine_galaxy(
        galaxy_ptr, &module_base, &galaxy_slot, &class_ref
    ) ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterCreateAndEnterSecondGalaxy(
    uint32_t galaxy_ptr, uint32_t second_seed, uint32_t player_race
) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t second;
    uint32_t star_count = 0;
    uint32_t star_list;

    if (second_seed == 0 || player_race > 7u ||
        InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) {
        return 0;
    }
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    second = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second != 0) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 2u;
    }

    second = ce_call_delphi_constructor(
        class_ref, module_base + CE_RVA_TGALAXY_CONSTRUCTOR
    );
    if (second == 0 || !ce_region_has_access((const void *)(uintptr_t)second, 0x1dcu, 1) ||
        *(const uint32_t *)(uintptr_t)second != class_ref) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }

    memcpy((void *)(uintptr_t)(second + 0x50u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x50u), 0x10u);
    memcpy((void *)(uintptr_t)(second + 0x188u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x188u), 0x27u);
    /* CE_RVA_TGALAXY_GENERATE_STARS (disassembled from the installed exe)
       reads its star-count target from [self+0x160] and `jle`-skips the
       whole generation loop when it is <= 0. The raw Delphi constructor
       call never sets it, so the field is zero on a synthetic instance;
       copy the real galaxy's target so the second arm generates the same
       number of stars. */
    memcpy((void *)(uintptr_t)(second + 0x160u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x160u), 0x4u);
    *(uint32_t *)(uintptr_t)(second + 0x1d4u) = second_seed;

    InterlockedExchange(&g_ce_old_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_second_generation_status, 1);
    ce_write_native_stage(1u, 0u);

    /* Runs synchronously on the caller (engine main) thread: the engine's
       Initialize/GenerateStars are not thread-safe, and the global Galaxy
       slot must only be swapped while no other engine code can observe it. */
    *galaxy_slot = second;
    ce_call_delphi_method(second, module_base + CE_RVA_TGALAXY_INITIALIZE);
    ce_write_native_stage(3u, 0u);
    ce_call_delphi_method_byte(second, player_race,
        module_base + CE_RVA_TGALAXY_GENERATE_STARS);
    /* GenerateStars clears and fills the list at [self+0x164] (a TList
       subclass: Clear() via its vtable, Add() per created star), not the
       field at +0x2c the previous ABI 8 build read back from; +0x2c is a
       different, unrelated pointer and always read back as empty. */
    star_list = *(uint32_t *)(uintptr_t)(second + 0x164u);
    if (ce_region_has_access((const void *)(uintptr_t)star_list, 12u, 0)) {
        star_count = *(uint32_t *)(uintptr_t)(star_list + 8u);
    }
    *galaxy_slot = galaxy_ptr;
    if (star_count != 0u) {
        InterlockedExchange(&g_ce_second_galaxy_ptr, (LONG)second);
        InterlockedExchange(&g_ce_second_snapshot_loaded, 0);
        InterlockedExchange(&g_ce_second_generation_status, 2);
        ce_write_native_stage(4u, star_count);
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 1;
    }
    /* Leave status at 0 (not a permanent 3) so the next day-skip retries
       instead of going silent forever; the failed `second` instance is
       abandoned (small leak, acceptable for this dev-only smoke module). */
    InterlockedExchange(&g_ce_second_generation_status, 0);
    ce_write_native_stage(5u, 0u);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 0;
}

/* Reads a Delphi TList-style count: the field holds a pointer to the list
   object, whose own +0x08 is its element count (the same shape
   CEAdapterCreateAndEnterSecondGalaxy already relies on for +0x164). */
static uint32_t ce_read_list_count(uint32_t owner, uint32_t field_offset) {
    uint32_t list;
    if (!ce_region_has_access((const void *)(uintptr_t)(owner + field_offset), 4u, 0)) return 0;
    list = *(const uint32_t *)(uintptr_t)(owner + field_offset);
    if (!ce_region_has_access((const void *)(uintptr_t)list, 12u, 0)) return 0;
    return *(const uint32_t *)(uintptr_t)(list + 8u);
}

static uint32_t ce_topology_hash_mix_u32(uint32_t hash, uint32_t value) {
    uint32_t byte_index;
    for (byte_index = 0u; byte_index < 4u; ++byte_index) {
        hash ^= (value >> (byte_index * 8u)) & 0xffu;
        hash *= 16777619u;
    }
    return hash;
}

/* A full native builder can produce the same counts for two genuinely
   different maps, so "20 sectors / 73 systems" is not sufficient proof of
   Second Home.  Hash only pointer-independent topology: list sizes and the
   generated X/Y bit patterns of every system in native list order.  Object
   addresses, names, owners and other mutable campaign state are deliberately
   excluded, making the value useful both before serialization and after a
   later load.  Zero means the topology could not be read safely. */
static uint32_t ce_galaxy_topology_hash(uint32_t galaxy) {
    uint32_t sector_count;
    uint32_t system_list;
    uint32_t system_array;
    uint32_t system_count;
    uint32_t system_index;
    uint32_t hash = 2166136261u;

    if (galaxy == 0u ||
        !ce_region_has_access(
            (const void *)(uintptr_t)(galaxy + 0x2cu), 4u, 0)) return 0u;
    sector_count = ce_read_list_count(galaxy, 0x164u);
    system_list = *(const uint32_t *)(uintptr_t)(galaxy + 0x2cu);
    if (!ce_region_has_access(
            (const void *)(uintptr_t)system_list, 12u, 0)) return 0u;
    system_array = *(const uint32_t *)(uintptr_t)(system_list + 4u);
    system_count = *(const uint32_t *)(uintptr_t)(system_list + 8u);
    if (sector_count == 0u || sector_count > 64u ||
        system_count == 0u || system_count > 512u ||
        !ce_region_has_access(
            (const void *)(uintptr_t)system_array, system_count * 4u, 0)) {
        return 0u;
    }

    hash = ce_topology_hash_mix_u32(hash, sector_count);
    hash = ce_topology_hash_mix_u32(hash, system_count);
    for (system_index = 0u; system_index < system_count; ++system_index) {
        uint32_t system = *(const uint32_t *)(uintptr_t)(
            system_array + system_index * 4u);
        uint32_t x_bits;
        uint32_t y_bits;
        if (!ce_region_has_access(
                (const void *)(uintptr_t)system, 0x1cu, 0)) return 0u;
        memcpy(&x_bits, (const void *)(uintptr_t)(system + 0x14u), 4u);
        memcpy(&y_bits, (const void *)(uintptr_t)(system + 0x18u), 4u);
        hash = ce_topology_hash_mix_u32(hash, system_index);
        hash = ce_topology_hash_mix_u32(hash, x_bits);
        hash = ce_topology_hash_mix_u32(hash, y_bits);
    }
    return hash != 0u ? hash : 1u;
}

/* See CE_RVA_NEWGAME_THREAD_CLASS's comment: builds Second Home by calling
   the engine's own complete new-game galaxy builder, instead of loading a
   snapshot of the player's own galaxy (which produced a byte-identical
   copy -- the actual reason Second Home never felt like a second map).

   Self is a synthetic, permanently-zeroed static instance rather than a
   real constructed TThread: constructing one properly would START AN OS
   THREAD, and the entire point here is to run Execute synchronously on the
   caller's thread and keep the galaxy it produces. Static (not malloc'd)
   so nothing can ever free it out from under the engine, and zeroed so any
   inherited TThread field Execute happens to consult (FTerminated and
   friends) reads as a clean default.

   DO NOT CALL THIS IN A LIVE GAME -- kept only as the documented record of
   what this engine entry point actually is. Live testing settled it: this
   is not a galaxy builder, it is the WHOLE new-game setup. Observed
   directly -- it replaced the player's own ship with a freshly rolled one
   of a different race (human player, Maloc ship), put the player on that
   new game's starting station, drew that game's initial course, and the
   process died as soon as the mismatched ship was rendered in the hangar.
   Saving/restoring CE_RVA_LOADING_FLAG_CELL and CE_RVA_NEWGAME_COUNTER_CELL
   does not and cannot address this: the builder CONSTRUCTS A NEW PLAYER
   OBJECT and republishes it, so there is no small set of globals to put
   back.

   The usable conclusion is still valuable, just different from the one
   first assumed: the galaxy it produces is genuinely complete (verified
   live: 20 sectors / 73 systems, versus 0 systems from the old
   GenerateStars-only attempt), so the engine CAN hand us a real second
   map -- but only from a context where clobbering the player is harmless,
   i.e. a new game the engine itself is starting. The supported way to
   obtain Second Home is therefore to capture such a galaxy through the
   existing SaveToStream hook and later LoadFromStream it into a bare
   galaxy during play, never to invoke this builder mid-session. */
uint32_t CE_CALL CEAdapterBuildFreshSecondGalaxy(uint32_t galaxy_ptr) {
    static uint32_t thread_instance[CE_NEWGAME_THREAD_INSTANCE_SIZE / 4u];
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t built;
    uint32_t sectors;
    uint32_t systems;
    uint32_t loading_flag_ptr;
    uint32_t counter_ptr;
    unsigned char saved_loading_flag = 0;
    uint32_t saved_counter = 0;
    int have_loading_flag = 0;
    int have_counter = 0;
    char report[192];
    int report_size;

    if (InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    if (!ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        ce_write_progress("build-fresh:abort:resolve-failed");
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    if ((uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0) != 0) {
        ce_write_progress("build-fresh:abort:already-built");
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 2u;
    }

    memset(thread_instance, 0, sizeof(thread_instance));
    thread_instance[0] = (uint32_t)(module_base + CE_RVA_NEWGAME_THREAD_CLASS);
    memcpy((unsigned char *)thread_instance + CE_NEWGAME_THREAD_SETTINGS_OFFSET,
        (const void *)(uintptr_t)(galaxy_ptr + 0x50u), CE_NEWGAME_THREAD_SETTINGS_SIZE);

    /* See CE_RVA_LOADING_FLAG_CELL's comment: capture the values BEFORE the
       builder overwrites them, so they can go back exactly as they were. */
    loading_flag_ptr = *(const uint32_t *)(module_base + CE_RVA_LOADING_FLAG_CELL);
    counter_ptr = *(const uint32_t *)(module_base + CE_RVA_NEWGAME_COUNTER_CELL);
    if (ce_region_has_access((const void *)(uintptr_t)loading_flag_ptr, 1u, 1)) {
        saved_loading_flag = *(const unsigned char *)(uintptr_t)loading_flag_ptr;
        have_loading_flag = 1;
    }
    if (ce_region_has_access((const void *)(uintptr_t)counter_ptr, 4u, 1)) {
        saved_counter = *(const uint32_t *)(uintptr_t)counter_ptr;
        have_counter = 1;
    }

    ce_write_progress("build-fresh:before-execute");
    ce_call_delphi_method((uint32_t)(uintptr_t)thread_instance,
        module_base + CE_RVA_NEWGAME_THREAD_EXECUTE);
    ce_write_progress("build-fresh:after-execute");

    if (have_loading_flag) {
        *(unsigned char *)(uintptr_t)loading_flag_ptr = saved_loading_flag;
    }
    if (have_counter) {
        *(uint32_t *)(uintptr_t)counter_ptr = saved_counter;
    }

    built = *galaxy_slot;
    /* Put the player's own arm back immediately, before anything else can
       observe the slot -- the builder publishes its result as the CURRENT
       galaxy, which is correct for a real new game and wrong for us. */
    *galaxy_slot = galaxy_ptr;

    if (built == 0u || built == galaxy_ptr ||
            !ce_region_has_access((const void *)(uintptr_t)built, 0x1dcu, 1) ||
            *(const uint32_t *)(uintptr_t)built != class_ref) {
        ce_write_progress("build-fresh:abort:bad-result");
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }

    /* Proof-of-completeness, logged rather than assumed: a genuinely built
       galaxy has BOTH lists populated. The old GenerateStars-only attempt
       filled +0x164 (sectors) and left +0x2c (systems) empty, which is
       precisely why it was unusable -- so a nonzero systems count is the
       thing that distinguishes this path from that failed one. */
    sectors = ce_read_list_count(built, 0x164u);
    systems = ce_read_list_count(built, 0x2cu);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"build-fresh\",\"galaxy\":%lu,\"sectors\":%lu,\"systems\":%lu}\r\n",
        (unsigned long)built, (unsigned long)sectors, (unsigned long)systems);
    if (report_size > 0) ce_write_text_marker("live-arm-switch.jsonl", report, (size_t)report_size);

    if (systems == 0u) {
        ce_write_progress("build-fresh:abort:no-systems");
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }

    InterlockedExchange(&g_ce_old_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_second_galaxy_ptr, (LONG)built);
    InterlockedExchange(&g_ce_second_snapshot_loaded, 1);
    InterlockedExchange(&g_ce_second_generation_status, 2);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1u;
}

uint32_t CE_CALL CEAdapterSecondGalaxyStatus(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_second_generation_status, 0, 0);
}

/* GenerateStars fills [self+0x164] with a fresh, independently-seeded star
   list -- fine for the empty Phase A swap test, but fatal for loading a
   real snapshot into it: LoadFromStream's later cross-reference reads
   (confirmed by disassembly, e.g. VA 0x00841e60 -- "find star by id in
   [self+0x164], raise CE_DELPHI_EXCEPTION_CODE if not found") expect the
   list to contain the EXACT star identities the snapshot references, not
   an unrelated independently-generated set. Skipping GenerateStars leaves
   the list empty-but-valid (Initialize alone is what allocates it as a
   real TList, confirmed safe in the earlier empty-galaxy Phase A work),
   so LoadFromStream's own star-reading stage can Add() its own entries
   with the correct identities instead of trying to match against ones
   that were never going to line up. */
uint32_t CE_CALL CEAdapterCreateBareSecondGalaxyForLoad(
    uint32_t galaxy_ptr, uint32_t second_seed
) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t second;
    uint32_t remembered_old;
    uint32_t remembered_second;

    /* This DLL's globals only ever reset via DllMain (a full process
       restart) -- exiting to the main menu and starting a NEW game
       WITHOUT restarting Rangers.exe keeps every one of them exactly as
       the previous game session left them. An earlier version of this
       check only looked for staleness while g_ce_active_arm==0, reasoning
       "if we're not inside the second galaxy, the old_galaxy_ptr should
       still be the live one" -- but if the player starts a new game
       WHILE active_arm was still 1 from the previous session (never
       pressed the hotkey to return first), active_arm stays stuck at 1
       and that gate never fires. Confirmed in practice: pressing the
       hotkey in the new session then took the "return" branch (since
       active_arm read as 1) and wrote the PREVIOUS session's stale
       old_galaxy_ptr into the engine's live galaxy slot, crashing shortly
       after. The only reliable signal is comparing the CURRENT galaxy_ptr
       against every pointer we remember from before, regardless of what
       active_arm currently claims -- if it matches neither, this is a
       session our state was never built for. */
    remembered_old = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    remembered_second = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (remembered_old != 0 && galaxy_ptr != remembered_old && galaxy_ptr != remembered_second) {
        InterlockedExchange(&g_ce_second_galaxy_ptr, 0);
        InterlockedExchange(&g_ce_second_generation_status, 0);
        InterlockedExchange(&g_ce_second_snapshot_loaded, 0);
        InterlockedExchange(&g_ce_active_arm, 0);
        InterlockedExchange(&g_ce_old_galaxy_ptr, 0);
        InterlockedExchange(&g_ce_last_swap_turn, -1);
        ce_write_progress("bare-second-galaxy:stale-session-reset");
    }

    second = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second != 0) return 2u;
    if (second_seed == 0 ||
        InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) {
        return 0;
    }
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }

    second = ce_call_delphi_constructor(
        class_ref, module_base + CE_RVA_TGALAXY_CONSTRUCTOR
    );
    if (second == 0 || !ce_region_has_access((const void *)(uintptr_t)second, 0x1dcu, 1) ||
        *(const uint32_t *)(uintptr_t)second != class_ref) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }

    memcpy((void *)(uintptr_t)(second + 0x50u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x50u), 0x10u);
    memcpy((void *)(uintptr_t)(second + 0x188u),
        (const void *)(uintptr_t)(galaxy_ptr + 0x188u), 0x27u);
    *(uint32_t *)(uintptr_t)(second + 0x1d4u) = second_seed;

    InterlockedExchange(&g_ce_old_galaxy_ptr, (LONG)galaxy_ptr);
    ce_write_progress("bare-second-galaxy:before-initialize");
    *galaxy_slot = second;
    ce_call_delphi_method(second, module_base + CE_RVA_TGALAXY_INITIALIZE);
    *galaxy_slot = galaxy_ptr;
    ce_write_progress("bare-second-galaxy:after-initialize");

    InterlockedExchange(&g_ce_second_galaxy_ptr, (LONG)second);
    InterlockedExchange(&g_ce_second_snapshot_loaded, 0);
    InterlockedExchange(&g_ce_second_generation_status, 2);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

uint32_t CE_CALL CEAdapterEnterReadySecondGalaxy(uint32_t galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t old_galaxy;
    uint32_t second_galaxy;

    /* No longer rejects when turn == g_ce_last_swap_turn. That guard was
       written for the Ctrl+Shift+1 test hotkey, which could fire from
       three duplicate OnKey panels for one keystroke with no other
       de-duplication; the real caller now is CEAdapterCompleteRegisteredPortal,
       which already atomically gates on portal_status (2->3) so this exact
       call only ever runs once per hole completion regardless of turn
       number. The turn check actively broke a legitimate return: entering
       and then returning without ever skipping a day in between means
       CurTurn() is IDENTICAL for both calls (the second galaxy's own clock
       starts wherever the snapshot's turn was, which can easily equal the
       old arm's current turn) -- confirmed live via
       "portal-complete-swap-failed" in live-arm-switch.jsonl, arm staying
       stuck on SECOND_HOME because this exact check rejected the return. */
    if (InterlockedCompareExchange(&g_ce_second_generation_status, 0, 0) != 2 ||
        InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (galaxy_ptr != old_galaxy || second_galaxy == 0 ||
        !ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        ce_write_progress("enter-ready:validation-failed");
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    ce_write_progress("enter-ready:before-swap");
    *galaxy_slot = second_galaxy;
    InterlockedExchange(&g_ce_active_arm, 1);
    InterlockedExchange(&g_ce_last_swap_turn, (LONG)turn);
    ce_write_progress("enter-ready:after-swap");
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

uint32_t CE_CALL CEAdapterReturnToOldGalaxy(uint32_t galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t old_galaxy;
    uint32_t second_galaxy;

    /* See CEAdapterEnterReadySecondGalaxy's own comment: the turn ==
       g_ce_last_swap_turn rejection was removed for the same reason -- it
       is the caller (CEAdapterCompleteRegisteredPortal) that now guards
       against duplicate processing, atomically, and this check broke a
       real same-turn return. */
    if (InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 || old_galaxy == 0 || galaxy_ptr != second_galaxy ||
        !ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    *galaxy_slot = old_galaxy;
    InterlockedExchange(&g_ce_active_arm, 0);
    InterlockedExchange(&g_ce_last_swap_turn, (LONG)turn);
    /* TRIED AND REVERTED: zeroing CE_RVA_CON_CONTEXT_CELL here and in
       CEAdapterEnterReadySecondGalaxy (on the theory that it is a stale
       cross-galaxy pointer left alive after every swap -- see that RVA's
       comment) caused an IMMEDIATE EAccessViolation on the very next hole
       entry, reading offset +0x4C off a null pointer -- confirmed live, OS
       crash dialog at RVA 0x0032fb43 (VA 0x0072fb43), 0xa5 bytes past
       CE_RVA_DAY_PROCESS_RAISE_CALL, i.e. still inside TGalaxy.NextDay.
       So this cell being null is only safe across the ONE narrow call
       (TCon's own constructor) that already guards it -- some OTHER code
       reachable from NextDay dereferences it with no null check at all.
       Whatever is actually stale here, blindly nulling this global is not
       a safe fix; the real source is still unidentified. */
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

uint32_t CE_CALL CEAdapterActiveArm(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_active_arm, 0, 0);
}

/* Confirmed live: dying in the arcade encounter (part of the native hole
   transit) triggers the engine's OWN "reload last autosave" recovery --
   a REAL load-game cycle, going through the engine's native LoadFromStream
   on a freshly, natively allocated TGalaxy, entirely bypassing
   CEAdapterEnterReadySecondGalaxy/ReturnToOldGalaxy. Our own bookkeeping
   (g_ce_active_arm, g_ce_old_galaxy_ptr, g_ce_second_galaxy_ptr) has no way
   to know this happened and is left pointing at whatever those pointers
   used to mean, which may now be stale, reused, or simply wrong --
   reported symptom was landing "in Second Home" after a death-reload with
   no memory of how. Called every Turn-code tick (like the existing raw
   pointer probe) with the CURRENT GalaxyPtr(): if it does not match what
   our own state says should be active, something changed the galaxy out
   from under us without going through our swap functions, and continuing
   to trust the stale bookkeeping risks acting on dangling pointers. Same
   reset shape as CEAdapterCreateBareSecondGalaxyForLoad's own stale-session
   check, just running continuously instead of only at the start of a new
   entry attempt. Skips the check entirely until g_ce_old_galaxy_ptr is
   ever set -- a session that has never touched Second Home has nothing to
   verify against yet. */
uint32_t CE_CALL CEAdapterCheckForExternalReload(uint32_t galaxy_ptr) {
    LONG active_arm = InterlockedCompareExchange(&g_ce_active_arm, 0, 0);
    uint32_t old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    uint32_t second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    uint32_t expected;
    if (old_galaxy == 0) return 0u;
    expected = active_arm == 1 ? second_galaxy : old_galaxy;
    if (expected == 0 || galaxy_ptr == expected) return 0u;
    InterlockedExchange(&g_ce_second_galaxy_ptr, 0);
    InterlockedExchange(&g_ce_second_generation_status, 0);
    InterlockedExchange(&g_ce_second_snapshot_loaded, 0);
    InterlockedExchange(&g_ce_active_arm, 0);
    InterlockedExchange(&g_ce_old_galaxy_ptr, 0);
    InterlockedExchange(&g_ce_last_swap_turn, -1);
    InterlockedExchange(&g_ce_portal_status, 0);
    InterlockedExchange(&g_ce_portal_hole_id, 0);
    InterlockedExchange(&g_ce_portal_galaxy_ptr, 0);
    ce_write_progress("external-reload-detected:state-reset");
    return 1u;
}

/* CEAdapterEnterReadySecondGalaxy (*galaxy_slot = second_galaxy, then back)
   twice reproduced a Rangers.exe crash in TGalaxy.NextDay right after being
   exercised in-game -- once after an empty-galaxy round trip, once even on
   a fresh save on the very next day-skip. The cause is not understood yet
   (TGalaxy.NextDay is not one of the three methods this adapter calls), so
   CE_MapSmoke stops calling Enter/Return/Abandon entirely and only marks
   the attempt terminal through this no-pointer-touching call: no engine
   memory is written, nothing is switched, only the internal status flag
   moves so the Turn code stops retrying. */
uint32_t CE_CALL CEAdapterMarkSecondGalaxyEntryDisabled(void) {
    InterlockedExchange(&g_ce_second_generation_status, 4);
    return 1;
}

/* RScript's own GalaxyStars()/GalaxyStar() read a count the post-generation
   orchestrator (name assignment, sectors, economy -- not yet reproduced
   here) fills in, so they report 0 for a galaxy GenerateStars alone built.
   The list GenerateStars actually Clear()s/Add()s into at [self+0x164] is a
   plain Delphi TList (FList ptr @+4, FCount @+8) and its entries are real
   star-class instances (built via the same class GenerateStars uses for
   every star it creates), so read that list directly instead of going
   through the not-yet-populated engine-level accessors. */
static uint32_t ce_read_generated_star_list(
    uint32_t galaxy_ptr, uint32_t *list_out, uint32_t *count_out
) {
    uint32_t star_list;
    uint32_t count;

    if (galaxy_ptr == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)galaxy_ptr, 0x168u, 0)) {
        return 0;
    }
    star_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x164u);
    if (!ce_region_has_access((const void *)(uintptr_t)star_list, 12u, 0)) {
        return 0;
    }
    count = *(const uint32_t *)(uintptr_t)(star_list + 8u);
    *list_out = star_list;
    *count_out = count;
    return 1;
}

uint32_t CE_CALL CEAdapterGetGeneratedStarCount(uint32_t galaxy_ptr) {
    uint32_t star_list;
    uint32_t count;
    if (!ce_read_generated_star_list(galaxy_ptr, &star_list, &count)) return 0;
    return count;
}

uint32_t CE_CALL CEAdapterGetGeneratedStarByIndex(uint32_t galaxy_ptr, uint32_t index) {
    uint32_t star_list;
    uint32_t count;
    uint32_t array_ptr;

    if (!ce_read_generated_star_list(galaxy_ptr, &star_list, &count) || index >= count) {
        return 0;
    }
    array_ptr = *(const uint32_t *)(uintptr_t)(star_list + 4u);
    if (!ce_region_has_access((const void *)(uintptr_t)array_ptr, (index + 1u) * 4u, 0)) {
        return 0;
    }
    return *(const uint32_t *)(uintptr_t)(array_ptr + index * 4u);
}

/* TransferShip only accepts a "system"/"planet"/"station" instance as its
   destination (three IsA checks against fixed class references; see the
   validator at VA 0x6403d9). None of the classes GenerateStars builds pass
   that check, which is exactly the "invalid destination" error the real
   game raised in-game. Rather than reproduce the full LoadFromStream
   deserializer that would normally populate real, fully-formed instances
   of the first class (TCon, at [galaxy+0x2c]), construct one bare instance
   directly via the engine's own constructor -- it passes the IsA check
   regardless of whether its other fields are populated -- and copy a
   position from the real galaxy's own first Con so it does not render at
   (0,0). Untested whether downstream code (map rendering, sector lookup)
   tolerates the otherwise-blank instance; that is the next unknown. */
/* Pure read-only diagnostic: reports whether the TCon class-ref cell looks
   like a valid pointer yet, without ever constructing anything. Written
   after CEAdapterCreateSecondDestination crashed for real on turn 1 of a
   brand new game -- need to know whether/when this cell settles before
   trying construction again. Safe to call every turn. */
static uint32_t ce_probe_cell(uintptr_t cell_address, uint32_t *value_out) {
    uint32_t value;
    if (!ce_region_has_access((const void *)cell_address, 4u, 0)) {
        *value_out = 0;
        return 0;
    }
    value = *(const uint32_t *)cell_address;
    *value_out = value;
    return value != 0 && ce_region_has_access((const void *)(uintptr_t)value, 4u, 0);
}

uint32_t CE_CALL CEAdapterProbeConClass(uint32_t old_galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref, con_class_ok;
    uint32_t context_value, context_ok;
    uint32_t sublist_class_ref, sublist_ok;
    uint32_t subobj_class_ref, subobj_ok;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[400];
    int payload_size;
    HANDLE file;
    DWORD written = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    con_class_ok = ce_probe_cell(module_base + CE_RVA_TCON_CLASS_CELL, &con_class_ref);
    context_ok = ce_probe_cell(module_base + CE_RVA_CON_CONTEXT_CELL, &context_value);
    sublist_ok = ce_probe_cell(module_base + CE_RVA_CON_SUBLIST_CLASS_CELL, &sublist_class_ref);
    subobj_ok = ce_probe_cell(module_base + CE_RVA_CON_SUBOBJ_CLASS_CELL, &subobj_class_ref);

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\con-class-probe.jsonl", marker_dir) < 0) {
        return 0;
    }
    payload_size = snprintf(payload, sizeof(payload),
        "{\"turn\":%u,\"con_class_ref\":%u,\"con_class_ok\":%s,"
        "\"context_value\":%u,\"context_ok\":%s,"
        "\"sublist_class_ref\":%u,\"sublist_ok\":%s,"
        "\"subobj_class_ref\":%u,\"subobj_ok\":%s}\r\n",
        turn, con_class_ref, con_class_ok ? "true" : "false",
        context_value, context_ok ? "true" : "false",
        sublist_class_ref, sublist_ok ? "true" : "false",
        subobj_class_ref, subobj_ok ? "true" : "false");
    if (payload_size <= 0 || (size_t)payload_size >= sizeof(payload)) return 0;
    file = CreateFileA(marker_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)payload_size, &written, NULL) || !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return (con_class_ok && sublist_ok && subobj_ok) ? 1u : 0u;
}

/* [0x882580] is a pointer-to-int used throughout TGalaxy.LoadFromStream /
   TCon's nested loader as a save-format version gate (compared against
   thresholds like 0x65, 0x7a, 0x85, 0x9e as the format grew over time).
   Need the exact runtime value once: guessing wrong about which side of a
   version gate we're on shifts every subsequent stream read by however
   many bytes that branch reads, corrupting the entire rest of a
   hand-built buffer. Pure read-only, writes nothing, safe every turn. */
uint32_t CE_CALL CEAdapterProbeSaveFormatVersion(uint32_t old_galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t version_cell_ok, version_ptr, version_ptr_ok, version_value;
    char payload[128];
    int payload_size;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    version_cell_ok = ce_probe_cell(module_base + 0x00482580u, &version_ptr);
    version_ptr_ok = version_cell_ok &&
        ce_region_has_access((const void *)(uintptr_t)version_ptr, 4u, 0);
    version_value = version_ptr_ok ? *(const uint32_t *)(uintptr_t)version_ptr : 0;

    payload_size = snprintf(payload, sizeof(payload),
        "{\"turn\":%u,\"version_ptr_ok\":%s,\"version_value\":%u}\r\n",
        turn, version_ptr_ok ? "true" : "false", version_value);
    if (payload_size > 0) {
        ce_write_text_marker("save-format-version.jsonl", payload, (size_t)payload_size);
    }
    return version_ptr_ok ? 1u : 0u;
}

/* Untested hypothesis for why TCon's constructor keeps corrupting shared
   state regardless of which individual side effect gets patched around:
   our synthetic galaxy never has [galaxy+0xc4] (a list of 0x78-byte
   per-race records, count at [galaxy+0x5c]) populated the way
   TGalaxy.LoadFromStream's header section builds it on a real load, and
   something in TCon's ~6 nested sub-constructors may expect to find it
   there. [galaxy+0xc4]'s records are built via a raw GetMem-style
   allocator (VA 0x4067a0), not a Delphi constructor -- no VMT to set up --
   so cloning them from the real galaxy is a plain, low-risk memcpy per
   record, guarded the same way as the rest of this file. Called once
   before ever attempting Con construction. */
/* Isolated test for which (if either) of TCon's two nested sub-object
   classes faults when constructed alone, outside TCon's constructor
   entirely -- narrows down whether the earlier repeat crash is really
   about one specific class or about the calling context in general.
   Each attempt is independently SEH-guarded so one faulting doesn't stop
   the other from being tried, and progress is logged before/after each. */
uint32_t CE_CALL CEAdapterProbeSubobjectConstruction(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t sublist_class_ref, subobj_class_ref;
    uint32_t sublist_result = 0, subobj_result = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("subobj-probe-abort:resolve-failed");
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_CON_SUBLIST_CLASS_CELL), 4u, 0) ||
        !ce_region_has_access((const void *)(module_base + CE_RVA_CON_SUBOBJ_CLASS_CELL), 4u, 0)) {
        ce_write_progress("subobj-probe-abort:class-cells-unreadable");
        return 0;
    }
    sublist_class_ref = *(const uint32_t *)(module_base + CE_RVA_CON_SUBLIST_CLASS_CELL);
    subobj_class_ref = *(const uint32_t *)(module_base + CE_RVA_CON_SUBOBJ_CLASS_CELL);

    ce_ensure_veh_installed();

    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("subobj-probe:sublist-class-FAULTED");
    } else {
        InterlockedExchange(&g_ce_guard_active, 1);
        ce_write_progress("subobj-probe:before-sublist-class");
        sublist_result = ce_call_delphi_constructor(
            sublist_class_ref, module_base + CE_RVA_GENERIC_CTOR_TRAMPOLINE);
        InterlockedExchange(&g_ce_guard_active, 0);
        ce_write_progress(sublist_result != 0
            ? "subobj-probe:sublist-class-ok"
            : "subobj-probe:sublist-class-returned-null");
    }

    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("subobj-probe:subobj-class-FAULTED");
    } else {
        InterlockedExchange(&g_ce_guard_active, 1);
        ce_write_progress("subobj-probe:before-subobj-class");
        subobj_result = ce_call_delphi_constructor(
            subobj_class_ref, module_base + CE_RVA_GENERIC_CTOR_TRAMPOLINE);
        InterlockedExchange(&g_ce_guard_active, 0);
        ce_write_progress(subobj_result != 0
            ? "subobj-probe:subobj-class-ok"
            : "subobj-probe:subobj-class-returned-null");
    }

    return (sublist_result != 0 ? 1u : 0u) | (subobj_result != 0 ? 2u : 0u);
}

/* Both of TCon's nested classes now confirmed safe to construct alone
   (CEAdapterProbeSubobjectConstruction). CEAdapterCreateSecondDestination
   has never once logged "after-construct" -- every attempt faults inside
   TCon's own constructor (VA 0x8482f8) before returning. This isolates
   the very first thing that constructor does: allocate a bare TCon-sized
   instance via the *generic* trampoline (same one used for the nested
   classes) instead of TCon's own full constructor body, skipping all 10
   nested constructions and every field initializer. If this succeeds,
   the fault is somewhere in TCon's constructor logic itself, not in the
   base allocation. */
uint32_t CE_CALL CEAdapterProbeBareConAllocation(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref;
    uint32_t result = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("bare-con-abort:resolve-failed");
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_TCON_CLASS_CELL), 4u, 0)) {
        ce_write_progress("bare-con-abort:class-cell-unreadable");
        return 0;
    }
    con_class_ref = *(const uint32_t *)(module_base + CE_RVA_TCON_CLASS_CELL);

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("bare-con-probe:FAULTED");
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);
    ce_write_progress("bare-con-probe:before");
    result = ce_call_delphi_constructor(
        con_class_ref, module_base + CE_RVA_GENERIC_CTOR_TRAMPOLINE);
    InterlockedExchange(&g_ce_guard_active, 0);
    ce_write_progress(result != 0 ? "bare-con-probe:ok" : "bare-con-probe:returned-null");
    return result;
}

/* The static (on-disk) value of the TCon class-ref cell is 0x00838fb8, a
   normal in-module VMT address -- but the *runtime* value read by every
   probe so far has been a heap address (order of 0x0c400000), nowhere near
   the module's mapped range (0x400000-0x8d1000). Something patches this
   cell at runtime, presumably a DLC/plugin-style VMT clone-and-override --
   a known Delphi pattern. Dump raw dwords around the runtime VMT pointer
   (covering the usual negative-offset VMT slot range, matching Delphi's
   classic layout) so they can be diffed against the static file bytes at
   the original address offline, to find exactly which slot differs. */
uint32_t CE_CALL CEAdapterDumpConVmt(uint32_t old_galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char marker_path[MAX_PATH];
    char payload[2048];
    int offset;
    int written_chars;
    int32_t index;
    HANDLE file;
    DWORD written = 0;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_TCON_CLASS_CELL), 4u, 0)) {
        return 0;
    }
    con_class_ref = *(const uint32_t *)(module_base + CE_RVA_TCON_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)(con_class_ref - 0x60u), 0x80u, 0)) {
        return 0;
    }

    if (GetTempPathA(MAX_PATH, temp_path) == 0) return 0;
    if (snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) < 0) return 0;
    if (!CreateDirectoryA(marker_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    if (snprintf(marker_path, sizeof(marker_path), "%s\\con-vmt-dump.json", marker_dir) < 0) {
        return 0;
    }
    written_chars = snprintf(payload, sizeof(payload),
        "{\"turn\":%u,\"con_class_ref\":%u,\"words\":[", turn, con_class_ref);
    if (written_chars <= 0) return 0;
    offset = written_chars;
    /* Offsets -0x60..+0x1c relative to the class ref, 4 bytes at a time:
       covers the classic Delphi VMT negative slot table plus a little
       past the pointer itself. */
    for (index = -0x60; index <= 0x1c; index += 4) {
        uint32_t value = *(const uint32_t *)(uintptr_t)((int32_t)con_class_ref + index);
        written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset,
            index == -0x60 ? "%u" : ",%u", value);
        if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
        offset += written_chars;
    }
    written_chars = snprintf(payload + offset, sizeof(payload) - (size_t)offset, "]}\r\n");
    if (written_chars <= 0 || (size_t)(offset + written_chars) >= sizeof(payload)) return 0;
    offset += written_chars;

    file = CreateFileA(marker_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(file, payload, (DWORD)offset, &written, NULL) || !FlushFileBuffers(file)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return written == (DWORD)offset ? 1u : 0u;
}

uint32_t CE_CALL CEAdapterCloneRaceRecords(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t second_galaxy;
    uint32_t old_count;
    uint32_t old_list, old_array;
    uint32_t new_list;
    uint32_t index;
    uint32_t cloned = 0;

    if (old_galaxy_ptr == 0) {
        ce_write_progress("race-clone-abort:no-old-galaxy");
        return 0;
    }
    if (!ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("race-clone-abort:resolve-failed");
        return 0;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0) {
        ce_write_progress("race-clone-abort:no-second-galaxy");
        return 0;
    }
    if (!ce_region_has_access((const void *)(uintptr_t)second_galaxy, CE_GALAXY_RACE_LIST_OFFSET + 4u, 1) ||
        !ce_region_has_access((const void *)(uintptr_t)old_galaxy_ptr, CE_GALAXY_RACE_LIST_OFFSET + 4u, 0)) {
        ce_write_progress("race-clone-abort:base-fields-unreadable");
        return 0;
    }

    old_count = *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + CE_GALAXY_RACE_LIST_COUNT_OFFSET);
    old_list = *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + CE_GALAXY_RACE_LIST_OFFSET);
    new_list = *(const uint32_t *)(uintptr_t)(second_galaxy + CE_GALAXY_RACE_LIST_OFFSET);
    {
        char diag[160];
        int diag_size = snprintf(diag, sizeof(diag),
            "race-clone-fields old_count=%u old_list=%u new_list=%u",
            old_count, old_list, new_list);
        if (diag_size > 0) ce_write_progress(diag);
    }
    if (old_count == 0u) {
        ce_write_progress("race-clone-abort:old_count-is-zero");
        return 0;
    }
    if (!ce_region_has_access((const void *)(uintptr_t)old_list, 12u, 0)) {
        ce_write_progress("race-clone-abort:old_list-unreadable");
        return 0;
    }
    if (!ce_region_has_access((const void *)(uintptr_t)new_list, 8u, 0)) {
        ce_write_progress("race-clone-abort:new_list-unreadable");
        return 0;
    }
    if (*(const uint32_t *)(uintptr_t)(old_list + 8u) < old_count) {
        ce_write_progress("race-clone-abort:old_list-count-mismatch");
        return 0;
    }
    old_array = *(const uint32_t *)(uintptr_t)(old_list + 4u);
    if (!ce_region_has_access((const void *)(uintptr_t)old_array, old_count * 4u, 0)) {
        ce_write_progress("race-clone-abort:old_array-unreadable");
        return 0;
    }

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_progress("race-clone-recovered-from-fault");
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);

    for (index = 0; index < old_count; ++index) {
        uint32_t old_record = *(const uint32_t *)(uintptr_t)(old_array + index * 4u);
        uint32_t new_record;
        if (!ce_region_has_access((const void *)(uintptr_t)old_record, CE_RACE_RECORD_BYTES, 0)) {
            continue;
        }
        new_record = ce_call_raw_alloc(CE_RACE_RECORD_BYTES, module_base + CE_RVA_RAW_RECORD_ALLOC);
        if (new_record == 0 ||
            !ce_region_has_access((const void *)(uintptr_t)new_record, CE_RACE_RECORD_BYTES, 1)) {
            continue;
        }
        memcpy((void *)(uintptr_t)new_record, (const void *)(uintptr_t)old_record, CE_RACE_RECORD_BYTES);
        ce_call_delphi_method_dword(new_list, new_record, module_base + CE_RVA_LIST_ADD);
        ++cloned;
    }
    InterlockedExchange(&g_ce_guard_active, 0);
    {
        char summary[64];
        int summary_size = snprintf(summary, sizeof(summary),
            "race-clone-done cloned=%u of %u", cloned, old_count);
        if (summary_size > 0) ce_write_progress(summary);
    }

    if (cloned == old_count &&
        ce_region_has_access((const void *)(uintptr_t)second_galaxy, CE_GALAXY_RACE_LIST_COUNT_OFFSET + 4u, 1)) {
        *(uint32_t *)(uintptr_t)(second_galaxy + 0x58u) =
            *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + 0x58u);
        *(uint32_t *)(uintptr_t)(second_galaxy + CE_GALAXY_RACE_LIST_COUNT_OFFSET) = old_count;
    }
    return cloned;
}

uint32_t CE_CALL CEAdapterCreateSecondDestination(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t con_class_ref;
    uint32_t second_galaxy;
    uint32_t con_list;
    uint32_t new_con;
    uint32_t ref_list;
    uint32_t ref_count;
    uint32_t ref_array;
    uint32_t ref_con;
    float ref_x;
    float ref_y;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)second_galaxy, 0x30u, 0)) {
        return 0;
    }
    if (!ce_region_has_access((const void *)(module_base + CE_RVA_TCON_CLASS_CELL), 4u, 0)) {
        return 0;
    }
    con_class_ref = *(const uint32_t *)(module_base + CE_RVA_TCON_CLASS_CELL);
    /* The class-ref cell crashed the constructor on day 1 of a brand new
       game (write access violation inside the engine's own NewInstance),
       most likely because this global isn't populated that early in the
       engine's own bootstrap yet. Require it to at least look like a
       plausible readable pointer before ever calling into the constructor
       with it -- this alone would have turned that crash into a safe 0
       return instead of a corrupted write. */
    if (!ce_region_has_access((const void *)(uintptr_t)con_class_ref, 4u, 0)) {
        return 0;
    }

    con_list = *(const uint32_t *)(uintptr_t)(second_galaxy + 0x2cu);
    if (!ce_region_has_access((const void *)(uintptr_t)con_list, 12u, 0)) {
        return 0;
    }

    /* Root cause of the earlier real corruption (confirmed by disassembling
       TCon's constructor, VA 0x8482f8): when [0x88c288] (a "current
       context" global, non-null during normal play per
       CEAdapterProbeConClass's own field) is non-null, the constructor
       unconditionally increments a counter on it before the point where it
       later faulted. Recovering via longjmp never undid that increment,
       and something unrelated (TPlanet.RelationToRanger) went out of sync
       with it later. Temporarily null the context for the exact duration
       of the constructor call so it takes the guarded (no-op) branch
       instead, then restore it immediately -- including on the recovered-
       fault path, so a caught fault doesn't leave the context zeroed for
       the rest of the session. */
    {
        volatile uint32_t context_saved = 0;
        volatile uint32_t original_context = 0;
        uintptr_t context_cell = module_base + CE_RVA_CON_CONTEXT_CELL;

        if (ce_region_has_access((const void *)context_cell, 4u, 0)) {
            original_context = *(const uint32_t *)context_cell;
            context_saved = 1;
        }

        /* Everything from here on calls directly into Delphi-compiled
           engine code from this foreign-compiled frame; see the VEH/setjmp
           comment near the top of the file. Recover to a safe 0 return
           instead of crashing if anything inside faults, and log which
           step was last reached (con-build-progress.log) either way. */
        ce_ensure_veh_installed();
        if (setjmp(g_ce_recovery_point) != 0) {
            if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
                *(uint32_t *)context_cell = original_context;
            }
            ce_write_progress("recovered-from-fault");
            return 0;
        }
        InterlockedExchange(&g_ce_guard_active, 1);

        if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
            *(uint32_t *)context_cell = 0;
        }
        ce_write_progress("before-construct");
        new_con = ce_call_delphi_constructor(con_class_ref, module_base + CE_RVA_TCON_CONSTRUCTOR);
        ce_write_progress("after-construct");
        if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
            *(uint32_t *)context_cell = original_context;
        }
    }
    if (new_con == 0 || !ce_region_has_access((const void *)(uintptr_t)new_con, 0x20u, 1)) {
        InterlockedExchange(&g_ce_guard_active, 0);
        return 0;
    }

    ref_x = 0.0f;
    ref_y = 0.0f;
    if (ce_region_has_access((const void *)(uintptr_t)(old_galaxy_ptr + 0x2cu), 4u, 0)) {
        ref_list = *(const uint32_t *)(uintptr_t)(old_galaxy_ptr + 0x2cu);
        if (ce_region_has_access((const void *)(uintptr_t)ref_list, 12u, 0)) {
            ref_count = *(const uint32_t *)(uintptr_t)(ref_list + 8u);
            if (ref_count > 0u) {
                ref_array = *(const uint32_t *)(uintptr_t)(ref_list + 4u);
                if (ce_region_has_access((const void *)(uintptr_t)ref_array, 4u, 0)) {
                    ref_con = *(const uint32_t *)(uintptr_t)ref_array;
                    if (ce_region_has_access((const void *)(uintptr_t)ref_con, 0x1cu, 0)) {
                        memcpy(&ref_x, (const void *)(uintptr_t)(ref_con + 0x14u), sizeof(ref_x));
                        memcpy(&ref_y, (const void *)(uintptr_t)(ref_con + 0x18u), sizeof(ref_y));
                    }
                }
            }
        }
    }
    ref_x += 500.0f;
    memcpy((void *)(uintptr_t)(new_con + 0x14u), &ref_x, sizeof(ref_x));
    memcpy((void *)(uintptr_t)(new_con + 0x18u), &ref_y, sizeof(ref_y));
    ce_write_progress("after-position-copy");

    ce_call_delphi_method_dword(con_list, new_con, module_base + CE_RVA_LIST_ADD);
    ce_write_progress("after-add");
    InterlockedExchange(&g_ce_guard_active, 0);
    return new_con;
}

/* Empirical probe for the still-unexplained TGalaxy.NextDay crash mentioned
   on CEAdapterMarkSecondGalaxyEntryDisabled: rather than let the engine's
   own turn loop call NextDay on whatever the Galaxy slot points to (where a
   fault is an unguarded, unrecoverable process crash), call it ourselves
   under the VEH/setjmp guard so a fault is caught and reported instead of
   taking the game down. ce_write_fault_report logs the exact faulting EIP
   and (for access violations) the read/write address, which is enough to
   identify which field NextDay dereferences that GenerateStars alone never
   populated. Never touches the live Galaxy slot or g_ce_active_arm -- the
   engine keeps running the real galaxy throughout this call. */
uint32_t CE_CALL CEAdapterProbeNextDayOnSecondGalaxy(uint32_t old_galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t second_galaxy;

    if (old_galaxy_ptr == 0 ||
        !ce_resolve_engine_galaxy(old_galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 ||
        !ce_region_has_access((const void *)(uintptr_t)second_galaxy, 0x1dcu, 0)) {
        return 0;
    }

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        ce_write_fault_report("nextday-probe-FAULTED");
        InterlockedExchange(&g_ce_guard_active, 0);
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);
    ce_write_progress("nextday-probe:before");
    ce_call_delphi_method_byte(second_galaxy, 0u, module_base + CE_RVA_TGALAXY_NEXTDAY);
    ce_write_progress("nextday-probe:after-ok");
    InterlockedExchange(&g_ce_guard_active, 0);
    return 1;
}

/* GenerateStars alone leaves a TGalaxy missing everything the engine's own
   post-generation bootstrap fills in (name assignment, sectors, economy),
   so an entered-but-empty second galaxy will never become non-empty on a
   later day. Without this, status stays at 2 forever and CE_MapSmoke's Turn
   code re-enters/re-returns the same broken instance every single day-skip
   for the rest of the session -- repeatedly flipping the live engine's
   Galaxy slot in place, which is the likely cause of the save failure seen
   after this happened once. Mark the attempt terminal instead of retrying. */
uint32_t CE_CALL CEAdapterAbandonEmptySecondGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t old_galaxy;
    uint32_t second_galaxy;

    if (InterlockedCompareExchange(&g_ce_native_switch_lock, 1, 0) != 0) return 0;
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    old_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_old_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 || old_galaxy == 0 || galaxy_ptr != second_galaxy ||
        !ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_native_switch_lock, 0);
        return 0;
    }
    *galaxy_slot = old_galaxy;
    InterlockedExchange(&g_ce_active_arm, 0);
    InterlockedExchange(&g_ce_second_galaxy_ptr, 0);
    InterlockedExchange(&g_ce_second_generation_status, 4);
    InterlockedExchange(&g_ce_native_switch_lock, 0);
    return 1;
}

/* One-shot window-tree dump, purely for locating the star-map screen's
   window/control layout from inside the process (no cross-process handle
   permissions needed, unlike external inspection tools). Logs every
   top-level window owned by this process and its full child-window tree
   to process-windows.log so a map-screen button can be positioned and
   parented correctly.

   CRASH LESSON (froze the whole game hard enough that even the OS
   couldn't close it, forcing a taskkill and leaving the desktop briefly
   broken): plain GetWindowTextA sends a blocking WM_GETTEXT to windows not
   owned by the calling thread. If Rangers.exe has any window living on a
   thread other than the one running our Turn-script call (a loading/audio
   worker, for instance) and that thread is waiting on the main thread for
   anything, this deadlocks both threads permanently -- classic Win32 GUI
   deadlock, unrecoverable without killing the process. Use
   SendMessageTimeoutA with SMTO_ABORTIFHUNG instead, which bounds the wait
   and returns instead of hanging forever. GetClassNameA/GetWindowRect never
   send messages (class info and geometry live in kernel window-manager
   state), so they were never the risk. */
static void ce_get_window_title_safe(HWND hwnd, char *out, size_t out_size) {
    DWORD_PTR result = 0;
    LRESULT sent;
    out[0] = 0;
    sent = SendMessageTimeoutA(hwnd, WM_GETTEXT, (WPARAM)out_size, (LPARAM)out,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result);
    if (sent == 0) {
        out[0] = 0;
    } else {
        out[out_size - 1] = 0;
    }
}

static BOOL CALLBACK ce_enum_child_proc(HWND hwnd, LPARAM lparam) {
    char class_name[128];
    char title[256];
    RECT rect;
    char payload[640];
    int size;
    HWND parent;

    (void)lparam;
    class_name[0] = 0;
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    ce_get_window_title_safe(hwnd, title, sizeof(title));
    parent = GetParent(hwnd);
    if (!GetWindowRect(hwnd, &rect)) {
        rect.left = rect.top = rect.right = rect.bottom = 0;
    }
    size = snprintf(payload, sizeof(payload),
        "  CHILD hwnd=0x%08lx parent=0x%08lx id=%d class=\"%s\" title=\"%s\" visible=%d rect=(%ld,%ld,%ld,%ld)\r\n",
        (unsigned long)(uintptr_t)hwnd, (unsigned long)(uintptr_t)parent,
        GetDlgCtrlID(hwnd), class_name, title, IsWindowVisible(hwnd) ? 1 : 0,
        (long)rect.left, (long)rect.top, (long)rect.right, (long)rect.bottom);
    if (size > 0) {
        ce_write_text_marker("process-windows.log", payload, (size_t)size);
    }
    return TRUE;
}

static BOOL CALLBACK ce_enum_top_proc(HWND hwnd, LPARAM lparam) {
    DWORD pid = 0;
    char class_name[128];
    char title[256];
    RECT rect;
    char payload[640];
    int size;

    (void)lparam;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    class_name[0] = 0;
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    ce_get_window_title_safe(hwnd, title, sizeof(title));
    if (!GetWindowRect(hwnd, &rect)) {
        rect.left = rect.top = rect.right = rect.bottom = 0;
    }
    size = snprintf(payload, sizeof(payload),
        "TOP hwnd=0x%08lx class=\"%s\" title=\"%s\" visible=%d rect=(%ld,%ld,%ld,%ld)\r\n",
        (unsigned long)(uintptr_t)hwnd, class_name, title, IsWindowVisible(hwnd) ? 1 : 0,
        (long)rect.left, (long)rect.top, (long)rect.right, (long)rect.bottom);
    if (size > 0) {
        ce_write_text_marker("process-windows.log", payload, (size_t)size);
    }
    EnumChildWindows(hwnd, ce_enum_child_proc, 0);
    return TRUE;
}

uint32_t CE_CALL CEAdapterDumpProcessWindows(void) {
    char header[64];
    int size = snprintf(header, sizeof(header), "--- window dump ---\r\n");
    if (size > 0) {
        ce_write_text_marker("process-windows.log", header, (size_t)size);
    }
    EnumWindows(ce_enum_top_proc, 0);
    return 1;
}

/* Unambiguous, non-visual proof of whether the swap actually changes what
   the rest of the engine considers "the current galaxy": logs the raw
   pointer value the script's own GalaxyPtr() resolves to, every turn, so a
   before/after comparison across a single click doesn't depend on reading
   star names off a screenshot. */
uint32_t CE_CALL CEAdapterProbeRawGalaxyPointer(uint32_t galaxy_ptr, uint32_t turn) {
    char payload[96];
    int size = snprintf(payload, sizeof(payload),
        "{\"turn\":%lu,\"galaxy_ptr\":%lu,\"active_arm\":%ld}\r\n",
        (unsigned long)turn, (unsigned long)galaxy_ptr,
        (long)InterlockedCompareExchange(&g_ce_active_arm, 0, 0));
    if (size > 0) {
        ce_write_text_marker("raw-galaxy-pointer.jsonl", payload, (size_t)size);
    }
    return 1;
}

static uint32_t ce_mix32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

/* A live arm switch must keep every engine reference valid.  Replacing the
   global TGalaxy pointer was proven unsafe (NextDay retained references to
   the old object), so ABI 12 snapshots and edits the loaded TCon/sector
   objects in place.  Object identity, planets, fleets, ownership and route
   targets remain untouched; only the map-facing coordinates and names are
   swapped, and the original values can be restored exactly. */
typedef struct ce_live_con_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
    float old_x;
    float old_y;
    uint32_t sector_object;
    /* Old-arm baseline for StarOwner()==1 (Dominators), captured once per
       system_index by CEAdapterCaptureDominatorFlag (called from RScript,
       which is the only side that can call the real StarOwner() built-in --
       this DLL has no way to invoke RScript's own functions, only the
       reverse). dominator_captured guards the idempotent "first value seen
       wins" capture: the per-star Turn-code loop that calls this runs every
       turn, including while Second Home's own suppression is active, so
       without this guard a later (already-suppressed) read would silently
       overwrite the true baseline. */
    uint32_t old_dominator;
    uint32_t dominator_captured;
} ce_live_con_snapshot;

typedef struct ce_live_sector_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
    uint32_t second_name_index;
    uint32_t old_visible;
    uint32_t old_sector_number;
} ce_live_sector_snapshot;

static ce_live_con_snapshot *g_ce_live_cons = NULL;
static ce_live_sector_snapshot *g_ce_live_sectors = NULL;
static ce_live_sector_snapshot *g_ce_live_sector_rows = NULL;
static uint32_t g_ce_live_con_count = 0;
static uint32_t g_ce_live_mapped_system_count = 0;
static uint32_t g_ce_live_sector_count = 0;
static uint32_t g_ce_live_sector_row_count = 0;
static uint32_t g_ce_live_snapshot_galaxy = 0;

/* Planets and stations are not reachable from a single galaxy-level list the
   way stars are (see con_list at galaxy_ptr+0x2c); RScript already walks
   them per-system via StarPlanets()/StarRuins(), so it hands each pointer
   to the DLL one at a time instead. Capture is idempotent per pointer (a
   linear scan against the already-captured list), matching how
   ce_capture_live_arm itself is idempotent per galaxy. */
typedef struct ce_live_named_snapshot {
    uint32_t object;
    uint32_t old_name;
    uint32_t second_name;
} ce_live_named_snapshot;

#define CE_LIVE_NAMED_CAPACITY 1024u

static ce_live_named_snapshot *g_ce_live_planets = NULL;
static uint32_t g_ce_live_planet_count = 0;
static uint32_t g_ce_live_planet_capacity = 0;

static ce_live_named_snapshot *g_ce_live_stations = NULL;
static uint32_t g_ce_live_station_count = 0;
static uint32_t g_ce_live_station_capacity = 0;

/* The first 11 entries are the canonical anchor systems named in
   MASTER_SPEC_COMPLETE.md / docs/11_SECOND_HOME_GALAXY_MAP.md -- keep
   these exact strings. Everything after was a uniform "Adjective + Noun"
   procedural filler (72/72 entries), which does not match how vanilla
   actually names systems: the decompiled CFG/Rus/Lang.dat's own Star
   table is close to 100% single words, mostly real or invented star
   names ("Тарон", "Ригель", "Антарес", "Арктур", "Велес"), with maybe
   one two-word entry in fifty. Rebuilt the filler the same way. */
static const wchar_t *const g_ce_second_system_names[] = {
    L"Эльтанская Рана", L"Первый Приют", L"Ковчег-IV", L"Крепость Карх",
    L"Узел Без Лица", L"Люмен", L"Призма Единства", L"Пепельный рынок",
    L"Сиротское гнездо", L"Город Незажжённых", L"Врата Второго Дома",
    L"Горн", L"Шлак", L"Уголь", L"Копоть", L"Искра", L"Гарь", L"Зарево",
    L"Пепел", L"Сталь", L"Латунь", L"Ржавчина", L"Окалина", L"Клинок",
    L"Молот", L"Кузнец", L"Горнило", L"Тигель", L"Плавка", L"Литьё",
    L"Сплав", L"Закал", L"Клеймо", L"Оковы", L"Засека", L"Дозор",
    L"Стража", L"Караул", L"Рубеж", L"Порог", L"Предел", L"Грань",
    L"Излом", L"Разлом", L"Раскол", L"Шрам", L"Ожог", L"Пепелище",
    L"Тлен", L"Хмарь", L"Морок", L"Мгла", L"Сумрак", L"Полночь",
    L"Затмение", L"Немота", L"Бездна", L"Провал", L"Утёс", L"Гряда",
    L"Хребет", L"Терраса", L"Уступ", L"Ложбина", L"Впадина", L"Ущелье",
    L"Каньон", L"Затон", L"Плёс", L"Омут", L"Каргаш", L"Тарнов",
    L"Эльтас", L"Агилар", L"Медиан", L"Интель", L"Клиссар", L"Молотар",
    L"Кузнар", L"Наковаль", L"Три Клятвы", L"Стальной Полдень",
    L"Последний Горн",
    /* Two real stars, added on request: both are genuinely reported at
       neighboring-spiral-arm distances from Earth (not just "in that
       constellation's direction" -- checked against sourced distance
       estimates, not assumed). Ро Кассиопеи (Rho Cassiopeiae) is a yellow
       hypergiant at roughly Perseus Arm distance (~8000 ly); Эта Киля (Eta
       Carinae) is placed in the Carina-Sagittarius Arm at ~2.3 kpc. */
    L"Ро Кассиопеи", L"Эта Киля"
};

/* Sector names are NOT rewritten from inside this DLL at all: the sector
   label the map actually draws lives in a generic BlockPar-style config-tree
   "row" object (VMT 0x008273a8, one per Constellations.Name entry, found
   contiguous in memory 0x30 bytes apart) that is completely separate from
   the sector game object tracked below (g_ce_live_sectors) -- finding those
   rows needs a bounded memory scan, and the one in-process scan attempt
   ever tried for this (ce_find_live_sector_rows) correlated with a real
   TGalaxy.NextDay crash and was removed. Doing the exact same kind of scan
   from a separate OS process instead (tools/native/ce_sector_name_helper.c,
   invoked via CreateProcess in ce_spawn_sector_name_helper below) gets the
   same result without ever running on any of this process's own threads,
   so it cannot reproduce that crash. Its own vanilla/Second-Home name pools
   are the single source of truth now; nothing here duplicates them. First
   version of that pool was uniform two-word "Adjective + Noun" (matching
   the same mistake already fixed for stars/planets/stations) -- rebuilt to
   mostly single words once decompiled Constellations.Name showed vanilla's
   20 sector names are 100% single words, zero exceptions. */

/* Planet name pointer offset (+0x14) confirmed via ce_dump_named_object
   live dump: object_ptr+0x14 decoded as a valid immortal Unicode string
   ("Арнарик Гаудад", a real generated planet name) -- not guessed.

   The first version of this pool was uniformly "Adjective + Noun"
   (24/24 entries), which reads nothing like vanilla. Decompiled the real
   CFG/Rus/Lang.dat with BlockParEditor and checked the actual PlanetName
   table the game itself uses: every vanilla planet name is a single
   invented word (varying wildly in length -- "Умий", "Апр", "Заа" vs.
   "Эйманаполон", "Унганамерун"), occasionally hyphenated compounds for
   some races ("Пунэ-анита", "Толт-гаин"). No two-word phrase anywhere in
   that table. Rebuilt this pool the same way, using the project's own
   lore roots (Карх/Тарг/Эльтан/Агилл/Медиум/Интелл/Клиссан) as seeds
   instead of copying vanilla's literal syllables. */
static const wchar_t *const g_ce_second_planet_names[] = {
    L"Карх", L"Таргай", L"Эльтанис", L"Агиллок", L"Медиумар", L"Интеллон",
    L"Клиссен", L"Карходар", L"Таргейн", L"Мельдун", L"Ошган", L"Ривентак",
    L"Зорхим", L"Уннарай", L"Пельтагон", L"Крихой", L"Мандар-Ош",
    L"Тэл-Гуна", L"Орхеста", L"Иннарай", L"Гуртан", L"Веллиш", L"Санторай",
    L"Микдал"
};

/* Station/base name pointer offset (+0x08) confirmed via the same live
   dump: object_ptr+0x08 decoded as a valid immortal Unicode string
   ("Генератор", a real generated station name).

   Same fix as the planet pool: the real vanilla RuinName table (also
   pulled from the decompiled Lang.dat) mixes single evocative words
   ("Толстяк", "Депозит", "Магнат", "Валгалла", "Крюгер") with two-word
   phrases ("Красный барон", "Черная жемчужина", "Пиратская гавань") and
   the rare full phrase ("Заплати и лети"), roughly half and half. This
   pool now does the same instead of "Noun + genitive-noun" on every
   entry. */
static const wchar_t *const g_ce_second_station_names[] = {
    L"Кузня", L"Причал Эльтана", L"Форпост Карха", L"Маяк", L"Верфь Молота",
    L"Застава", L"Санктум", L"Наковальня", L"Пристань Двух Домов",
    L"Клятва", L"Редут Кузнецов", L"Бастион", L"Отражение",
    L"Цитадель Молчания", L"Резонанс", L"Форт Второго Восхода"
};

/* This 32-bit Delphi build stores the UTF-16 string byte length (BSTR-style),
   not the character count, in the dword immediately before its data.  The
   preceding dword is kept immortal so the engine can retain the pointer. */
static uint32_t ce_make_immortal_unicode(const wchar_t *text) {
    size_t length = wcslen(text);
    size_t bytes = 8u + (length + 1u) * sizeof(wchar_t);
    unsigned char *block = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (block == NULL || length > 0x7fffffffu) return 0;
    *(int32_t *)(void *)block = -1;
    *(uint32_t *)(void *)(block + 4u) = (uint32_t)(length * sizeof(wchar_t));
    memcpy(block + 8u, text, (length + 1u) * sizeof(wchar_t));
    return (uint32_t)(uintptr_t)(block + 8u);
}

/* Same immortal-refcount idea, but Delphi AnsiString layout: refcount at
   -8, length in BYTES (= chars, 1 byte each) at -4, then the data and a
   NUL. Needed because CE_RVA_GAME_LOAD_PATH_CELL is an ANSISTRING slot:
   TThreadGameLoad.Execute widens it with @WStrFromLStr before calling
   LoadGame (VA 0x0053c97a -> 0x00405f00, which reads the length at
   [src-4] and converts the buffer through MultiByteToWideChar at
   0x004055cc -- confirmed by disassembly). Storing a UTF-16 string there
   made the engine widen the already-wide bytes a second time, producing a
   path with a NUL wchar interleaved after every character -- caught live
   by the LoadGame diagnostics as reason=cannot-open-file with exactly that
   doubled pattern in the logged filename, while direct LoadGame calls
   (which skip the cell and its conversion) kept working. Conversion uses
   CP_ACP to match what MultiByteToWideChar will do on the way back. */
static uint32_t ce_make_immortal_ansi(const wchar_t *text) {
    int narrow_bytes = WideCharToMultiByte(
        CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
    unsigned char *block;
    if (narrow_bytes <= 1) return 0;
    block = (unsigned char *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, 8u + (size_t)narrow_bytes);
    if (block == NULL) return 0;
    *(int32_t *)(void *)block = -1;
    *(uint32_t *)(void *)(block + 4u) = (uint32_t)(narrow_bytes - 1);
    if (WideCharToMultiByte(CP_ACP, 0, text, -1,
            (char *)(block + 8u), narrow_bytes, NULL, NULL) != narrow_bytes) {
        HeapFree(GetProcessHeap(), 0, block);
        return 0;
    }
    return (uint32_t)(uintptr_t)(block + 8u);
}

/* Galaxy object names are owned WideString/BSTR fields, not borrowed
   call arguments.  They are released with SysFreeString when the object is
   destroyed, so the HeapAlloc-backed immortal strings used for transient
   engine calls are not valid here.  Allocate through OleAut32 and let the
   normal object destructor own the returned BSTR. */
static uint32_t ce_make_owned_widestring(const wchar_t *text) {
    typedef wchar_t *(WINAPI *ce_sys_alloc_string_len_fn)(
        const wchar_t *source, unsigned int length);
    static ce_sys_alloc_string_len_fn allocate_string = NULL;
    HMODULE oleaut32;
    size_t length;

    if (text == NULL) return 0u;
    if (allocate_string == NULL) {
        oleaut32 = GetModuleHandleW(L"oleaut32.dll");
        if (oleaut32 == NULL) oleaut32 = LoadLibraryW(L"oleaut32.dll");
        if (oleaut32 == NULL) return 0u;
        allocate_string = (ce_sys_alloc_string_len_fn)(
            void *)GetProcAddress(oleaut32, "SysAllocStringLen");
        if (allocate_string == NULL) return 0u;
    }
    length = wcslen(text);
    if (length > UINT_MAX) return 0u;
    return (uint32_t)(uintptr_t)allocate_string(text, (unsigned int)length);
}

static int ce_make_dual_newgame_paths(uintptr_t module_base) {
    uint32_t manager_slot;
    uint32_t manager;
    uint32_t turn_save_path = 0u;
    const wchar_t *turn_save_text;
    uint32_t path_bytes;
    size_t path_chars;
    size_t separator = (size_t)-1;
    size_t index;
    wchar_t first_path[1024];
    wchar_t second_path[1024];
    static const wchar_t first_name[] = L"CE_Eltan_FirstArm.sav";
    static const wchar_t second_name[] = L"CE_Eltan_SecondHome.sav";

    if (!ce_region_has_access(
            (const void *)(module_base + CE_RVA_SAVE_MANAGER_CELL), 4u, 0)) return 0;
    manager_slot = *(const uint32_t *)(module_base + CE_RVA_SAVE_MANAGER_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)manager_slot, 4u, 0)) return 0;
    manager = *(const uint32_t *)(uintptr_t)manager_slot;
    if (manager == 0u || !ce_region_has_access((const void *)(uintptr_t)manager, 4u, 0)) {
        return 0;
    }

    ce_call_delphi_eax_edx(
        manager, (uint32_t)(uintptr_t)&turn_save_path,
        module_base + CE_RVA_GET_TURN_SAVE_PATH);
    if (turn_save_path < 8u ||
        !ce_region_has_access((const void *)(uintptr_t)(turn_save_path - 4u), 4u, 0)) {
        return 0;
    }
    path_bytes = *(const uint32_t *)(uintptr_t)(turn_save_path - 4u);
    if ((path_bytes & 1u) != 0u || path_bytes == 0u ||
        path_bytes > (uint32_t)((1024u - 1u) * sizeof(wchar_t)) ||
        !ce_region_has_access((const void *)(uintptr_t)turn_save_path, path_bytes, 0)) {
        return 0;
    }
    path_chars = (size_t)path_bytes / sizeof(wchar_t);
    turn_save_text = (const wchar_t *)(uintptr_t)turn_save_path;
    for (index = 0u; index < path_chars; ++index) {
        if (turn_save_text[index] == L'\\' || turn_save_text[index] == L'/') {
            separator = index;
        }
    }
    if (separator == (size_t)-1 ||
        separator + 1u + wcslen(first_name) >= 1024u ||
        separator + 1u + wcslen(second_name) >= 1024u) {
        return 0;
    }

    memcpy(first_path, turn_save_text, (separator + 1u) * sizeof(wchar_t));
    memcpy(second_path, turn_save_text, (separator + 1u) * sizeof(wchar_t));
    wcscpy(first_path + separator + 1u, first_name);
    wcscpy(second_path + separator + 1u, second_name);
    g_ce_first_arm_save_path = ce_make_immortal_unicode(first_path);
    g_ce_second_home_save_path = ce_make_immortal_unicode(second_path);
    g_ce_first_arm_save_path_ansi = ce_make_immortal_ansi(first_path);
    g_ce_second_home_save_path_ansi = ce_make_immortal_ansi(second_path);
    return g_ce_first_arm_save_path != 0u && g_ce_second_home_save_path != 0u &&
        g_ce_first_arm_save_path_ansi != 0u && g_ce_second_home_save_path_ansi != 0u;
}

/* True when the immortal UnicodeString path names a file that already
   exists. Used to leave authored content alone. */
static int ce_file_exists(uint32_t path) {
    DWORD attributes;
    if (path == 0u) return 0;
    attributes = GetFileAttributesW((const wchar_t *)(uintptr_t)path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int ce_wait_for_save_writer(uintptr_t module_base) {
    uint32_t writer;
    uint32_t running;
    if (!ce_region_has_access(
            (const void *)(module_base + CE_RVA_SAVE_WRITER_OBJECT), 4u, 0)) return 0;
    writer = *(const uint32_t *)(module_base + CE_RVA_SAVE_WRITER_OBJECT);
    if (writer == 0u || !ce_region_has_access((const void *)(uintptr_t)writer, 4u, 0)) {
        return 0;
    }
    running = ce_call_delphi_eax_edx(
        writer, 0u, module_base + CE_RVA_THREAD_IS_RUNNING) & 0xffu;
    if (running != 0u) {
        ce_call_delphi_eax_edx(
            writer, 0xffffffffu, module_base + CE_RVA_THREAD_WAIT_FOR);
    }
    return 1;
}

static int ce_save_complete_game(
    uintptr_t module_base, uint32_t filename, const wchar_t *title
) {
    uint32_t title_string;
    uint32_t result;
    if (filename == 0u) return 0;
    title_string = ce_make_immortal_unicode(title);
    if (title_string == 0u) return 0;
    ce_call_delphi_method(0u, module_base + CE_RVA_PREPARE_SAVE_PREVIEWS);
    result = ce_call_delphi_eax_edx(
        filename, title_string, module_base + CE_RVA_SAVE_GAME) & 0xffu;
    if (result == 0u) return 0;
    return ce_wait_for_save_writer(module_base);
}

static int ce_load_complete_game(uintptr_t module_base, uint32_t filename) {
    if (filename == 0u) return 0;
    return (ce_call_delphi_eax_edx(
        filename, 0u, module_base + CE_RVA_LOAD_GAME) & 0xffu) != 0u;
}

static int ce_set_native_game_load_path(uintptr_t module_base, uint32_t filename) {
    uint32_t path_slot;
    if (filename == 0u ||
        !ce_region_has_access(
            (const void *)(module_base + CE_RVA_GAME_LOAD_PATH_CELL), 4u, 0)) {
        return 0;
    }
    path_slot = *(const uint32_t *)(module_base + CE_RVA_GAME_LOAD_PATH_CELL);
    if (!ce_region_has_access((void *)(uintptr_t)path_slot, 4u, 1)) return 0;
    /* filename is an immortal Delphi UnicodeString (refcount -1), so direct
       assignment is valid across the asynchronous GameLoad form.  Do not
       release the previous field here: the form owns its normal lifecycle,
       while our string deliberately remains valid for the process. */
    *(uint32_t *)(uintptr_t)path_slot = filename;
    return 1;
}

/* Runs in the engine's own TThreadCreateNewGame worker.  This is the one
   context in which invoking the complete new-game builder a second time is
   valid: the UI is already showing native loading, day simulation has not
   started, and replacement of every global object is expected.  Restoring
   the first arm uses the engine's full LoadGame lifecycle, never a raw
   pointer swap or a hand-picked list of globals. */
__attribute__((used)) static void CE_CALL ce_dual_newgame_execute(uint32_t self) {
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    uint32_t *galaxy_slot;
    uint32_t second_thread;
    uint32_t first_galaxy;
    uint32_t second_galaxy;
    uint32_t restored_galaxy;
    uint32_t sectors;
    uint32_t systems;
    uint32_t first_topology_hash;
    uint32_t second_topology_hash;
    uint32_t validated_topology_hash = 0u;
    unsigned char original_thread[CE_NEWGAME_THREAD_INSTANCE_SIZE];
    int first_saved = 0;
    int second_saved = 0;
    int second_loaded = 0;
    int second_normalized = 0;
    int restored = 0;
    char report[320];
    int report_size;

    if (g_ce_dual_newgame_trampoline == NULL) return;
    if (InterlockedCompareExchange(&g_ce_dual_newgame_running, 1, 0) != 0) {
        ce_call_delphi_method(self, (uintptr_t)g_ce_dual_newgame_trampoline);
        return;
    }
    InterlockedExchange(&g_ce_dual_newgame_status, 1);

    memset(original_thread, 0, sizeof(original_thread));
    if (self != 0u &&
        ce_region_has_access((const void *)(uintptr_t)self,
            CE_NEWGAME_THREAD_INSTANCE_SIZE, 0)) {
        /* The eight bytes at +0x2d are only the race-presence block copied
           into TGalaxy.  The remainder of the derived thread instance also
           contains the selected player race/difficulty and other new-game
           choices.  The earlier mid-session experiment copied only those
           eight bytes and demonstrably produced a Maloc player for a human
           setup.  Preserve the complete 0x4c instance instead; Execute runs
           synchronously on the same real worker, so its inherited thread
           handles still refer to the correct active thread. */
        memcpy(original_thread, (const void *)(uintptr_t)self,
            sizeof(original_thread));
    }

    ce_write_progress("dual-newgame:first:before-execute");
    ce_call_delphi_method(self, (uintptr_t)g_ce_dual_newgame_trampoline);
    ce_write_progress("dual-newgame:first:after-execute");

    if (!ce_region_has_access(
            (const void *)(module_base + CE_RVA_GALAXY_IMPORT_CELL), 4u, 0)) {
        InterlockedExchange(&g_ce_dual_newgame_status, 3);
        goto done;
    }
    galaxy_slot = *(uint32_t **)(module_base + CE_RVA_GALAXY_IMPORT_CELL);
    if (!ce_region_has_access(galaxy_slot, 4u, 0)) {
        InterlockedExchange(&g_ce_dual_newgame_status, 3);
        goto done;
    }
    first_galaxy = *galaxy_slot;
    if (first_galaxy == 0u || !ce_make_dual_newgame_paths(module_base)) {
        ce_write_progress("dual-newgame:abort:no-first-or-path");
        InterlockedExchange(&g_ce_dual_newgame_status, 3);
        goto done;
    }
    first_topology_hash = ce_galaxy_topology_hash(first_galaxy);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"dual-newgame-first-built\",\"galaxy\":%lu,"
        "\"sectors\":%lu,\"systems\":%lu,\"topology_hash\":%lu}\r\n",
        (unsigned long)first_galaxy,
        (unsigned long)ce_read_list_count(first_galaxy, 0x164u),
        (unsigned long)ce_read_list_count(first_galaxy, 0x2cu),
        (unsigned long)first_topology_hash);
    if (report_size > 0) ce_write_text_marker(
        "dual-newgame.jsonl", report, (size_t)report_size);
    if (first_topology_hash == 0u) {
        ce_write_progress("dual-newgame:abort:first-topology-unreadable");
        InterlockedExchange(&g_ce_dual_newgame_status, 3);
        goto done;
    }

    ce_write_progress("dual-newgame:first:before-save");
    first_saved = ce_save_complete_game(
        module_base, g_ce_first_arm_save_path, L"Дети Эльтан: Первый рукав");
    ce_write_progress(first_saved
        ? "dual-newgame:first:saved" : "dual-newgame:abort:first-save-failed");
    if (!first_saved) {
        InterlockedExchange(&g_ce_dual_newgame_status, 3);
        goto done;
    }

    /* An authored Second Home must not be overwritten. Once the sidecar
       exists on disk it is treated as prepared content -- built from a
       normally-saved game and rewritten into Eltan naming by
       tools/make_second_home.py -- and generation is skipped entirely.
       That also retires the second Execute, which was the source of both
       the wrong captain (it builds a whole new game, its own player
       included) and the half-built interface those mid-generation saves
       carried. Regenerating is then a matter of deleting the file. */
    if (ce_file_exists(g_ce_second_home_save_path)) {
        ce_write_progress("dual-newgame:second:prepared-file-kept");
        InterlockedExchange(&g_ce_dual_newgame_status, 2);
        goto restore_first;
    }

    /* Execute is a real TThread method, not a plain builder function.
       Neither a byte-copied fake object nor re-entering Execute on the
       already-completed worker is valid.  Construct the same class exactly
       as the stock new-game form does, suspended, then copy only the
       derived TThreadCreateNewGame fields.  The base TThread portion stays
       owned by this new object and therefore has a valid handle, thread ID,
       event and runtime identity.  The spare worker intentionally remains
       suspended for the rest of this short loading process; destroying a
       suspended Delphi TThread from inside another worker is riskier than
       leaking this single process-lifetime object. */
    second_thread = ce_call_delphi_constructor(
        (uint32_t)(module_base + CE_RVA_NEWGAME_THREAD_CLASS),
        module_base + CE_RVA_TTHREAD_CONSTRUCTOR);
    if (second_thread == 0u ||
        !ce_region_has_access((void *)(uintptr_t)second_thread,
            CE_NEWGAME_THREAD_INSTANCE_SIZE, 1)) {
        ce_write_progress("dual-newgame:second:thread-create-failed");
        InterlockedExchange(&g_ce_dual_newgame_status, 4);
        goto restore_first;
    }
    memcpy(
        (void *)(uintptr_t)(second_thread + CE_NEWGAME_THREAD_DERIVED_OFFSET),
        original_thread + CE_NEWGAME_THREAD_DERIVED_OFFSET,
        CE_NEWGAME_THREAD_INSTANCE_SIZE - CE_NEWGAME_THREAD_DERIVED_OFFSET);
    ce_write_progress("dual-newgame:second:before-execute");
    ce_call_delphi_method(second_thread, (uintptr_t)g_ce_dual_newgame_trampoline);
    ce_write_progress("dual-newgame:second:after-execute");
    second_galaxy = *galaxy_slot;
    sectors = ce_read_list_count(second_galaxy, 0x164u);
    systems = ce_read_list_count(second_galaxy, 0x2cu);
    second_topology_hash = ce_galaxy_topology_hash(second_galaxy);
    if (second_galaxy == 0u || second_galaxy == first_galaxy || systems == 0u) {
        ce_write_progress("dual-newgame:second:invalid");
        InterlockedExchange(&g_ce_dual_newgame_status, 4);
        goto restore_first;
    }
    if (second_topology_hash == 0u ||
        second_topology_hash == first_topology_hash) {
        ce_write_progress(second_topology_hash == 0u
            ? "dual-newgame:second:topology-unreadable"
            : "dual-newgame:second:topology-duplicate");
        InterlockedExchange(&g_ce_dual_newgame_status, 4);
        goto restore_first;
    }

    /* Keep the serialized system identifiers produced by the native
       builder.  TCon.Name participates in cross-object save references, so
       rewriting only that field makes the sidecar readable in the save
       menu but crashes during a full load.  Second Home's display names are
       applied only after its valid native sidecar has loaded. */
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"dual-newgame-second-built\",\"galaxy\":%lu,"
        "\"sectors\":%lu,\"systems\":%lu,\"topology_hash\":%lu,"
        "\"first_topology_hash\":%lu,\"topology_distinct\":true}\r\n",
        (unsigned long)second_galaxy, (unsigned long)sectors,
        (unsigned long)systems, (unsigned long)second_topology_hash,
        (unsigned long)first_topology_hash);
    if (report_size > 0) ce_write_text_marker(
        "dual-newgame.jsonl", report, (size_t)report_size);

    /* Each Execute builds a COMPLETE new game, player included -- proven
       live when calling it mid-session swapped a human player's ship for a
       freshly rolled Maloc one.  Saving straight after the second Execute
       therefore produced a sidecar holding that second game's OWN player:
       flying the portal loaded a stranger's party rather than the same
       captain in another arm, and left the post-load state inconsistent
       enough that the loading form's own gate (VA 0x007029A4) refused to
       nominate a follow-up form, ending the process cleanly with exit code
       0 -- the "silent crash" that produced no fault and no event-log
       entry.  Give Second Home the FIRST arm's player instead: park the
       freshly built galaxy behind the engine's own keep-alive flag, load
       the first arm back (restoring its player), then point the galaxy cell
       at the second arm and save that combination. */
    /* REVERTED, kept as the record of a disproved approach.  Each Execute
       builds a COMPLETE new game, player included, so the second sidecar
       holds that second game's own player rather than the traveller's --
       the user's own hypothesis, and correct.  The obvious repair, "load
       the first arm back to recover its player, then point the galaxy cell
       at the second arm and save that pair", does NOT work: SaveGame then
       died with `List index out of bounds (-1)` (raise site VA 0x0041622b,
       captured live by the first-chance trace) and returned false, so no
       sidecar was written at all and the portal could never arm.

       The -1 is a "not found in list" result used as an index, and it
       proves the mismatch is not merely the player's own star reference: a
       save is one coherent object graph -- galaxy, every ship, quests and
       player all cross-referencing each other -- so substituting the galaxy
       underneath a state loaded from the other arm breaks references all
       over that graph, not in one repairable place.  Carrying a captain
       between arms therefore has to copy the traveller's attributes into
       the destination world, not swap a world under a party. */
    ce_write_progress("dual-newgame:second:before-save");
    second_saved = ce_save_complete_game(
        module_base, g_ce_second_home_save_path, L"Дети Эльтан: Второй Дом");
    ce_write_progress(second_saved
        ? "dual-newgame:second:saved" : "dual-newgame:second-save-failed");
    if (!second_saved) InterlockedExchange(&g_ce_dual_newgame_status, 4);
    if (second_saved) {
        /* A successful SaveGame call only proves that the asynchronous
           writer accepted the buffers.  It does not prove that the complete
           state produced by a second new-game builder in the same process is
           independently loadable.  Validate it now, while the stock loading
           screen is already active and before the player can enter the
           world.  Re-saving the successfully reconstructed state also
           removes any process-local registry residue left by running two
           builders back-to-back. */
        ce_write_progress("dual-newgame:second:before-validation-load");
        second_loaded = ce_load_complete_game(
            module_base, g_ce_second_home_save_path);
        ce_write_progress(second_loaded
            ? "dual-newgame:second:validation-loaded"
            : "dual-newgame:second:validation-load-failed");
        if (second_loaded && ce_region_has_access(galaxy_slot, 4u, 0)) {
            validated_topology_hash =
                ce_galaxy_topology_hash(*galaxy_slot);
        }
        if (second_loaded &&
            validated_topology_hash == second_topology_hash) {
            ce_write_progress("dual-newgame:second:before-normalize-save");
            second_normalized = ce_save_complete_game(
                module_base, g_ce_second_home_save_path,
                L"Дети Эльтан: Второй Дом");
            ce_write_progress(second_normalized
                ? "dual-newgame:second:normalized"
                : "dual-newgame:second:normalize-save-failed");
        } else if (second_loaded) {
            ce_write_progress(
                "dual-newgame:second:validation-topology-mismatch");
        }
        if (!second_normalized) {
            InterlockedExchange(&g_ce_dual_newgame_status, 4);
        }
        report_size = snprintf(report, sizeof(report),
            "{\"status\":\"dual-newgame-second-validation\","
            "\"loaded\":%d,\"normalized\":%d,"
            "\"expected_topology_hash\":%lu,"
            "\"loaded_topology_hash\":%lu}\r\n",
            second_loaded, second_normalized,
            (unsigned long)second_topology_hash,
            (unsigned long)validated_topology_hash);
        if (report_size > 0) ce_write_text_marker(
            "dual-newgame.jsonl", report, (size_t)report_size);
    }

restore_first:
    ce_write_progress("dual-newgame:first:before-restore");
    restored = ce_load_complete_game(module_base, g_ce_first_arm_save_path);
    restored_galaxy = ce_region_has_access(galaxy_slot, 4u, 0) ? *galaxy_slot : 0u;
    ce_write_progress(restored
        ? "dual-newgame:first:restored" : "dual-newgame:first:restore-failed");
    if (restored && second_normalized && restored_galaxy != 0u) {
        InterlockedExchange(&g_ce_old_galaxy_ptr, 0);
        InterlockedExchange(&g_ce_second_galaxy_ptr, 0);
        InterlockedExchange(&g_ce_second_snapshot_loaded, 0);
        InterlockedExchange(&g_ce_active_arm, 0);
        InterlockedExchange(&g_ce_dual_newgame_status, 2);
    } else if (!restored) {
        InterlockedExchange(&g_ce_dual_newgame_status, 5);
    }
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"dual-newgame-finished\",\"first_saved\":%d,"
        "\"second_saved\":%d,\"second_loaded\":%d,"
        "\"second_normalized\":%d,\"restored\":%d,"
        "\"restored_galaxy\":%lu}\r\n",
        first_saved, second_saved, second_loaded, second_normalized,
        restored, (unsigned long)restored_galaxy);
    if (report_size > 0) ce_write_text_marker(
        "dual-newgame.jsonl", report, (size_t)report_size);

done:
    InterlockedExchange(&g_ce_dual_newgame_running, 0);
}

#if defined(__i386__)
__attribute__((naked)) static void ce_dual_newgame_execute_hook(void) {
    __asm__ volatile(
        "pushl %eax\n\t"
        "call _ce_dual_newgame_execute\n\t"
        "addl $4, %esp\n\t"
        "ret\n\t"
    );
}
#endif

static int ce_install_dual_newgame_hook(void) {
#if defined(__i386__)
    static const unsigned char execute_signature[8] = {
        0x55, 0x8b, 0xec, 0xb9, 0x6e, 0x00, 0x00, 0x00
    };
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    unsigned char *target;
    unsigned char *trampoline;
    unsigned char patch[8];
    int32_t relative;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;

    hook_state = InterlockedCompareExchange(
        &g_ce_dual_newgame_hook_installed, -1, 0);
    if (hook_state == 1) return 1;
    if (hook_state != 0 || module_base == 0u) return 0;
    dos = (IMAGE_DOS_HEADER *)module_base;
    if (!ce_region_has_access(dos, sizeof(*dos), 0) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE) goto fail;
    nt = (IMAGE_NT_HEADERS32 *)(module_base + (uintptr_t)dos->e_lfanew);
    if (!ce_region_has_access(nt, sizeof(*nt), 0) ||
        nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != CE_RANGERS_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != CE_RANGERS_IMAGE_SIZE) goto fail;

    target = (unsigned char *)(module_base + CE_RVA_NEWGAME_THREAD_EXECUTE);
    if (!ce_region_has_access(target, sizeof(execute_signature), 0) ||
        memcmp(target, execute_signature, sizeof(execute_signature)) != 0) goto fail;
    trampoline = (unsigned char *)VirtualAlloc(
        NULL, 13u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) goto fail;
    memcpy(trampoline, target, 8u);
    trampoline[8] = 0xe9;
    relative = (int32_t)((target + 8u) - (trampoline + 13u));
    memcpy(trampoline + 9u, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 13u);

    patch[0] = 0xe9;
    relative = (int32_t)(
        (unsigned char *)(uintptr_t)ce_dual_newgame_execute_hook - (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    memset(patch + 5u, 0x90, sizeof(patch) - 5u);
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        goto fail;
    }
    g_ce_dual_newgame_trampoline = trampoline;
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old_protect, &ignored_protect);
    InterlockedExchange(&g_ce_dual_newgame_hook_installed, 1);
    ce_write_progress("dual-newgame:hook-installed");
    return 1;

fail:
    InterlockedExchange(&g_ce_dual_newgame_hook_installed, 0);
    ce_write_progress("dual-newgame:hook-install-failed");
    return 0;
#else
    return 0;
#endif
}

static size_t ce_ansistring_to_hex(
    uint32_t string_ptr, char *output, size_t output_capacity
) {
    uint32_t byte_length;
    uint32_t index;
    size_t used = 0u;
    if (output_capacity != 0u) output[0] = '\0';
    if (string_ptr < 4u || output_capacity < 3u ||
        !ce_region_has_access(
            (const void *)(uintptr_t)(string_ptr - 4u), 4u, 0)) return 0u;
    byte_length = *(const uint32_t *)(uintptr_t)(string_ptr - 4u);
    if (byte_length > 160u) byte_length = 160u;
    if (byte_length != 0u &&
        !ce_region_has_access(
            (const void *)(uintptr_t)string_ptr, byte_length, 0)) return 0u;
    for (index = 0u; index < byte_length && used + 2u < output_capacity; ++index) {
        int written = snprintf(
            output + used, output_capacity - used, "%02x",
            (unsigned)*(const unsigned char *)(uintptr_t)(string_ptr + index));
        if (written != 2) break;
        used += 2u;
    }
    return used;
}

__attribute__((used)) static void CE_CALL ce_log_package_file_open_result(
    const uint32_t *saved_arguments, uint32_t result
) {
    uint32_t object;
    uint32_t path_ptr = 0u;
    uint32_t entry_index = 0xffffffffu;
    uint32_t reference_count = 0u;
    char path_hex[321];
    char report[760];
    int report_size;

    if (saved_arguments == NULL) return;
    object = saved_arguments[0];
    if (ce_region_has_access(
            (const void *)(uintptr_t)(object + 0x0cu), 4u, 0)) {
        entry_index = *(const uint32_t *)(uintptr_t)(object + 0x04u);
        reference_count = *(const uint32_t *)(uintptr_t)(object + 0x08u);
        path_ptr = *(const uint32_t *)(uintptr_t)(object + 0x0cu);
    }
    ce_ansistring_to_hex(path_ptr, path_hex, sizeof(path_hex));
    report_size = snprintf(
        report, sizeof(report),
        "{\"status\":\"package-file-open\",\"pid\":%lu,\"tid\":%lu,"
        "\"object\":%lu,\"flag\":%lu,\"result\":%lu,"
        "\"entry_index\":%lu,\"reference_count\":%lu,"
        "\"path_ptr\":%lu,\"path_hex\":\"%s\"}\r\n",
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetCurrentThreadId(),
        (unsigned long)object,
        (unsigned long)(saved_arguments[1] & 0xffu),
        (unsigned long)(result & 0xffu),
        (unsigned long)entry_index,
        (unsigned long)reference_count,
        (unsigned long)path_ptr, path_hex);
    if (report_size > 0) ce_write_text_marker(
        "package-file-opens.jsonl", report, (size_t)report_size);
}

#if defined(__i386__)
__attribute__((naked)) static void ce_package_file_open_hook(void) {
    __asm__ volatile(
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "pushl %eax\n\t"
        "call *_g_ce_package_file_open_trampoline\n\t"
        "pushl %eax\n\t"
        "leal 4(%esp), %ecx\n\t"
        "pushl %eax\n\t"
        "pushl %ecx\n\t"
        "call _ce_log_package_file_open_result\n\t"
        "addl $8, %esp\n\t"
        "popl %eax\n\t"
        "addl $12, %esp\n\t"
        "ret\n\t"
    );
}
#endif

static int ce_install_package_file_open_trace_hook(void) {
#if defined(__i386__)
    static const unsigned char signature[11] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xf4, 0x33, 0xc9,
        0x89, 0x4d, 0xf4
    };
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    unsigned char *target;
    unsigned char *trampoline;
    unsigned char patch[11];
    int32_t relative;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;

    hook_state = InterlockedCompareExchange(
        &g_ce_package_file_open_hook_installed, -1, 0);
    if (hook_state == 1) return 1;
    if (hook_state != 0 || module_base == 0u) return 0;
    target = (unsigned char *)(module_base + CE_RVA_PACKAGE_FILE_OPEN);
    if (!ce_region_has_access(target, sizeof(signature), 0) ||
        memcmp(target, signature, sizeof(signature)) != 0) goto fail;
    trampoline = (unsigned char *)VirtualAlloc(
        NULL, 16u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) goto fail;
    memcpy(trampoline, target, 11u);
    trampoline[11] = 0xe9;
    relative = (int32_t)((target + 11u) - (trampoline + 16u));
    memcpy(trampoline + 12u, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 16u);
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xe9;
    relative = (int32_t)(
        (unsigned char *)(uintptr_t)ce_package_file_open_hook - (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    if (!VirtualProtect(
            target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        goto fail;
    }
    g_ce_package_file_open_trampoline = trampoline;
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old_protect, &ignored_protect);
    InterlockedExchange(&g_ce_package_file_open_hook_installed, 1);
    return 1;

fail:
    InterlockedExchange(&g_ce_package_file_open_hook_installed, 0);
    return 0;
#else
    return 0;
#endif
}

static void ce_log_process_exit_request(
    const char *kind, uint32_t value, const uint32_t *stack_pointer
) {
    MEMORY_BASIC_INFORMATION region;
    uintptr_t stack_end = 0u;
    char report[1800];
    size_t used;
    uint32_t index;
    int written;

    if (stack_pointer != NULL &&
        VirtualQuery(stack_pointer, &region, sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT) {
        stack_end = (uintptr_t)region.BaseAddress + region.RegionSize;
    }
    written = snprintf(
        report, sizeof(report),
        "{\"status\":\"%s\",\"pid\":%lu,\"tid\":%lu,\"value\":%lu,"
        "\"game_stack\":[",
        kind,
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetCurrentThreadId(),
        (unsigned long)value);
    if (written <= 0 || (size_t)written >= sizeof(report)) return;
    used = (size_t)written;
    for (index = 0u; index < 256u && stack_pointer != NULL &&
         (uintptr_t)(stack_pointer + index + 1u) <= stack_end; ++index) {
        uint32_t address = stack_pointer[index];
        if (address < 0x00401000u || address >= 0x00875000u) continue;
        written = snprintf(
            report + used, sizeof(report) - used,
            "%s{\"index\":%lu,\"address\":%lu}",
            used != (size_t)snprintf(
                NULL, 0,
                "{\"status\":\"%s\",\"pid\":%lu,\"tid\":%lu,\"value\":%lu,"
                "\"game_stack\":[",
                kind,
                (unsigned long)GetCurrentProcessId(),
                (unsigned long)GetCurrentThreadId(),
                (unsigned long)value) ? "," : "",
            (unsigned long)index, (unsigned long)address);
        if (written <= 0 || (size_t)written >= sizeof(report) - used) break;
        used += (size_t)written;
    }
    if (used + 4u >= sizeof(report)) return;
    memcpy(report + used, "]}\r\n", 4u);
    ce_write_text_marker("process-exits.jsonl", report, used + 4u);
}

__attribute__((used)) static void CE_CALL ce_log_exit_process_stack(
    const uint32_t *stack_pointer, uint32_t exit_code
) {
    ce_log_process_exit_request("ExitProcess", exit_code, stack_pointer);
}

__attribute__((used)) static void CE_CALL ce_log_post_quit_message_stack(
    const uint32_t *stack_pointer, uint32_t exit_code
) {
    ce_log_process_exit_request("PostQuitMessage", exit_code, stack_pointer);
}

#if defined(__i386__)
__attribute__((naked)) static void ce_exit_process_trace_hook(void) {
    __asm__ volatile(
        "movl %esp, %eax\n\t"
        "pushl 4(%esp)\n\t"
        "pushl %eax\n\t"
        "call _ce_log_exit_process_stack\n\t"
        "addl $8, %esp\n\t"
        "jmp *_g_ce_exit_process_original\n\t"
    );
}

__attribute__((naked)) static void ce_post_quit_message_trace_hook(void) {
    __asm__ volatile(
        "movl %esp, %eax\n\t"
        "pushl 4(%esp)\n\t"
        "pushl %eax\n\t"
        "call _ce_log_post_quit_message_stack\n\t"
        "addl $8, %esp\n\t"
        "jmp *_g_ce_post_quit_message_original\n\t"
    );
}
#endif

static int ce_install_process_exit_trace_hooks(void) {
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    void **exit_process_cell;
    void **post_quit_message_cell;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;

    hook_state = InterlockedCompareExchange(
        &g_ce_process_exit_trace_installed, -1, 0);
    if (hook_state == 1) return 1;
    if (hook_state != 0 || module_base == 0u) return 0;
    exit_process_cell =
        (void **)(module_base + CE_RVA_EXIT_PROCESS_IAT);
    post_quit_message_cell =
        (void **)(module_base + CE_RVA_POST_QUIT_MESSAGE_IAT);
    if (!ce_region_has_access(exit_process_cell, sizeof(void *), 0) ||
        !ce_region_has_access(post_quit_message_cell, sizeof(void *), 0)) {
        goto fail;
    }
    g_ce_exit_process_original =
        (VOID (WINAPI *)(UINT))*exit_process_cell;
    g_ce_post_quit_message_original =
        (VOID (WINAPI *)(int))*post_quit_message_cell;
    if (g_ce_exit_process_original == NULL ||
        g_ce_post_quit_message_original == NULL) goto fail;
    if (!VirtualProtect(
            exit_process_cell, sizeof(void *), PAGE_READWRITE,
            &old_protect)) goto fail;
    *exit_process_cell = (void *)(uintptr_t)ce_exit_process_trace_hook;
    VirtualProtect(
        exit_process_cell, sizeof(void *), old_protect, &ignored_protect);
    if (!VirtualProtect(
            post_quit_message_cell, sizeof(void *), PAGE_READWRITE,
            &old_protect)) goto fail;
    *post_quit_message_cell =
        (void *)(uintptr_t)ce_post_quit_message_trace_hook;
    VirtualProtect(
        post_quit_message_cell, sizeof(void *), old_protect,
        &ignored_protect);
    InterlockedExchange(&g_ce_process_exit_trace_installed, 1);
    return 1;

fail:
    InterlockedExchange(&g_ce_process_exit_trace_installed, 0);
    return 0;
}

__attribute__((used)) static void CE_CALL ce_log_unhandled_exception(
    const uint32_t *stack_pointer, const EXCEPTION_RECORD *record
) {
    MEMORY_BASIC_INFORMATION region;
    uintptr_t stack_end = 0u;
    char report[2200];
    size_t used;
    uint32_t index;
    uint32_t parameter_count;
    int written;
    int first = 1;

    if (record == NULL || !ce_region_has_access(
            record, sizeof(EXCEPTION_RECORD), 0)) return;
    if (stack_pointer != NULL &&
        VirtualQuery(stack_pointer, &region, sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT) {
        stack_end = (uintptr_t)region.BaseAddress + region.RegionSize;
    }
    parameter_count = record->NumberParameters;
    if (parameter_count > EXCEPTION_MAXIMUM_PARAMETERS) {
        parameter_count = EXCEPTION_MAXIMUM_PARAMETERS;
    }
    written = snprintf(
        report, sizeof(report),
        "{\"status\":\"delphi-unhandled\",\"pid\":%lu,\"tid\":%lu,"
        "\"code\":%lu,\"flags\":%lu,\"address\":%lu,\"parameters\":[",
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetCurrentThreadId(),
        (unsigned long)record->ExceptionCode,
        (unsigned long)record->ExceptionFlags,
        (unsigned long)(uintptr_t)record->ExceptionAddress);
    if (written <= 0 || (size_t)written >= sizeof(report)) return;
    used = (size_t)written;
    for (index = 0u; index < parameter_count; ++index) {
        written = snprintf(
            report + used, sizeof(report) - used,
            "%s%lu", index == 0u ? "" : ",",
            (unsigned long)record->ExceptionInformation[index]);
        if (written <= 0 || (size_t)written >= sizeof(report) - used) return;
        used += (size_t)written;
    }
    written = snprintf(report + used, sizeof(report) - used, "],\"game_stack\":[");
    if (written <= 0 || (size_t)written >= sizeof(report) - used) return;
    used += (size_t)written;
    for (index = 0u; index < 384u && stack_pointer != NULL &&
         (uintptr_t)(stack_pointer + index + 1u) <= stack_end; ++index) {
        uint32_t address = stack_pointer[index];
        if (address < 0x00401000u || address >= 0x00875000u) continue;
        written = snprintf(
            report + used, sizeof(report) - used,
            "%s{\"index\":%lu,\"address\":%lu}",
            first ? "" : ",", (unsigned long)index, (unsigned long)address);
        if (written <= 0 || (size_t)written >= sizeof(report) - used) break;
        used += (size_t)written;
        first = 0;
    }
    if (used + 4u >= sizeof(report)) return;
    memcpy(report + used, "]}\r\n", 4u);
    ce_write_text_marker("unhandled-exceptions.jsonl", report, used + 4u);
}

#if defined(__i386__)
__attribute__((naked)) static void ce_unhandled_exception_trace_hook(void) {
    __asm__ volatile(
        "movl %esp, %eax\n\t"
        "pushl 4(%esp)\n\t"
        "pushl %eax\n\t"
        "call _ce_log_unhandled_exception\n\t"
        "addl $8, %esp\n\t"
        "jmp *_g_ce_unhandled_exception_trampoline\n\t"
    );
}
#endif

static int ce_install_unhandled_exception_trace_hook(void) {
#if defined(__i386__)
    static const unsigned char signature[11] = {
        0x8b, 0x44, 0x24, 0x04, 0xf7, 0x40, 0x04, 0x06,
        0x00, 0x00, 0x00
    };
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    unsigned char *target;
    unsigned char *trampoline;
    unsigned char patch[11];
    int32_t relative;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;

    hook_state = InterlockedCompareExchange(
        &g_ce_unhandled_exception_hook_installed, -1, 0);
    if (hook_state == 1) return 1;
    if (hook_state != 0 || module_base == 0u) return 0;
    target = (unsigned char *)(
        module_base + CE_RVA_DELPHI_UNHANDLED_EXCEPTION);
    if (!ce_region_has_access(target, sizeof(signature), 0) ||
        memcmp(target, signature, sizeof(signature)) != 0) goto fail;
    trampoline = (unsigned char *)VirtualAlloc(
        NULL, 16u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) goto fail;
    memcpy(trampoline, target, 11u);
    trampoline[11] = 0xe9;
    relative = (int32_t)((target + 11u) - (trampoline + 16u));
    memcpy(trampoline + 12u, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 16u);
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xe9;
    relative = (int32_t)(
        (unsigned char *)(uintptr_t)ce_unhandled_exception_trace_hook -
        (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    if (!VirtualProtect(
            target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        goto fail;
    }
    g_ce_unhandled_exception_trampoline = trampoline;
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old_protect, &ignored_protect);
    InterlockedExchange(&g_ce_unhandled_exception_hook_installed, 1);
    return 1;

fail:
    InterlockedExchange(&g_ce_unhandled_exception_hook_installed, 0);
    return 0;
#else
    return 0;
#endif
}

/* Idempotent per object_ptr (linear scan against what is already captured,
   same style as ce_capture_live_arm's own once-per-galaxy guard). Allocates
   its backing array lazily on first use, capped at CE_LIVE_NAMED_CAPACITY --
   comfortably above any real galaxy's planet/station count, so this never
   grows unbounded or needs realloc. */
static uint32_t ce_capture_named_entity(
        ce_live_named_snapshot **array_ptr, uint32_t *count_ptr, uint32_t *capacity_ptr,
        uint32_t object_ptr, uint32_t name_offset,
        const wchar_t *const *pool, uint32_t pool_size) {
    ce_live_named_snapshot *array;
    uint32_t index;
    uint32_t old_name;
    uint32_t second_name;
    if (object_ptr == 0u) return 0u;
    array = *array_ptr;
    for (index = 0; index < *count_ptr; ++index) {
        if (array[index].object == object_ptr) return 1u;
    }
    if (array == NULL) {
        array = (ce_live_named_snapshot *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            CE_LIVE_NAMED_CAPACITY * sizeof(*array));
        if (array == NULL) return 0u;
        *array_ptr = array;
        *capacity_ptr = CE_LIVE_NAMED_CAPACITY;
    }
    if (*count_ptr >= *capacity_ptr) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(object_ptr + name_offset), 4u, 1))
        return 0u;
    old_name = *(const uint32_t *)(uintptr_t)(object_ptr + name_offset);
    second_name = ce_make_immortal_unicode(pool[*count_ptr % pool_size]);
    if (second_name == 0u) return 0u;
    array[*count_ptr].object = object_ptr;
    array[*count_ptr].old_name = old_name;
    array[*count_ptr].second_name = second_name;
    ++(*count_ptr);
    return 1u;
}

static int ce_read_plain_list(uint32_t list, uint32_t *array_out, uint32_t *count_out) {
    uint32_t array;
    uint32_t count;
    if (!ce_region_has_access((const void *)(uintptr_t)list, 12u, 0)) return 0;
    array = *(const uint32_t *)(uintptr_t)(list + 4u);
    count = *(const uint32_t *)(uintptr_t)(list + 8u);
    if (count == 0u || count > 10000u ||
        !ce_region_has_access((const void *)(uintptr_t)array, count * 4u, 0)) return 0;
    *array_out = array;
    *count_out = count;
    return 1;
}

static volatile LONG g_ce_star_con_compared = 0;

/* CONFIRMED (2026-07-24, cross-checked against the independent GalaxyEye
   project -- github.com/bXana/GalaxyEye, a native C++ SR HD injector whose
   own Galaxy.h/Sector.h agree with this): self+0x164 is the SECTOR list,
   self+0x2c is the STAR ("system"/TCon) list -- the reverse of this
   function's own local variable names (tstar_* / tcon_*) and the original
   docs/TGALAXY_LOADFROMSTREAM_FORMAT.md "Stage 1/Stage 2" labels, which
   both called +0x164 "TStar". The object this whole project has actually
   been reading/writing as "the star" (name at +0x10, X/Y at +0x14/+0x18,
   the one GalaxyStar()/StarToCon() resolve to) is the +0x2c list's TCon --
   real stars/systems, not sectors. +0x164 is what GalaxySectors() returns,
   which is why its own would-be "name" field write (+0x04) never affected
   any displayed label: sector names are drawn by list POSITION, not a
   field on the object. GalaxyEye's sector-visibility offset (+9 as a
   whole byte) also matches this project's own independently-found
   "+0x08, bit 0x100" (bit 8 of a word starting at +0x08 IS byte +0x09) --
   independent corroboration, not a coincidence. Variable names below keep
   their original tstar_* / tcon_* spelling to avoid an unrelated rename
   across this large file; treat tstar_* as "the self+0x164 list" (sectors)
   and tcon_* as "the self+0x2c list" (stars/systems) when reading this
   function. Read-only, bounded, one-shot -- same safety class as every
   other list read this session. */
uint32_t CE_CALL CEAdapterCompareStarConCounts(uint32_t galaxy_ptr) {
    uint32_t tstar_list, tstar_array, tstar_count = 0;
    uint32_t tcon_list, tcon_array, tcon_count = 0;
    char report[192];
    int report_size;
    int tstar_ok, tcon_ok;

    if (InterlockedCompareExchange(&g_ce_star_con_compared, 1, 0) != 0) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x164u), 4u, 0) ||
        !ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x2cu), 4u, 0)) {
        return 0u;
    }
    tstar_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x164u);
    tcon_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    tstar_ok = ce_read_plain_list(tstar_list, &tstar_array, &tstar_count);
    tcon_ok = ce_read_plain_list(tcon_list, &tcon_array, &tcon_count);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"star-con-count-compare\",\"tstar_ok\":%s,\"tstar_count\":%u,"
        "\"tcon_ok\":%s,\"tcon_count\":%u,\"equal\":%s}\r\n",
        tstar_ok ? "true" : "false", tstar_count,
        tcon_ok ? "true" : "false", tcon_count,
        (tstar_ok && tcon_ok && tstar_count == tcon_count) ? "true" : "false");
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return 1u;
}

static int ce_live_read_wide_string(uint32_t pointer, const wchar_t **text_out,
        uint32_t *characters_out) {
    uint32_t byte_length;
    const wchar_t *text;
    uint32_t characters, index;
    int has_cyrillic = 0;
    if (pointer < 0x10000u ||
            !ce_region_has_access((const void *)(uintptr_t)(pointer - 4u), 4u, 0)) return 0;
    byte_length = *(const uint32_t *)(uintptr_t)(pointer - 4u);
    if (byte_length < 2u || byte_length > 96u || (byte_length & 1u) != 0u ||
            !ce_region_has_access((const void *)(uintptr_t)pointer, byte_length, 0)) return 0;
    characters = byte_length / 2u;
    text = (const wchar_t *)(uintptr_t)pointer;
    for (index = 0; index < characters; ++index) {
        wchar_t ch = text[index];
        if (ch >= 0x0400 && ch <= 0x052fu) has_cyrillic = 1;
        else if (ch != L' ' && ch != L'-' && ch != L'\'' &&
                !(ch >= L'0' && ch <= L'9')) return 0;
    }
    if (!has_cyrillic) return 0;
    *text_out = text;
    *characters_out = characters;
    return 1;
}

/* ce_live_row_index_matches and the full-address-space scan that used to
   call it (ce_find_live_sector_rows) were removed: see the comment in
   ce_capture_live_arm where the scan used to run for why -- a synchronous
   VirtualQuery walk of the entire process address space, executed exactly
   once per galaxy on the very first Turn-code invocation, is the prime
   suspect for a reproducible TGalaxy.NextDay crash on turn 0/1 of a new
   game, and the UI-row refresh it fed was cosmetic, not authoritative. */

static int ce_compare_live_sector_objects(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    uint32_t a_id = *(const uint32_t *)(uintptr_t)(a + 4u);
    uint32_t b_id = *(const uint32_t *)(uintptr_t)(b + 4u);
    return a_id < b_id ? -1 : a_id > b_id ? 1 : 0;
}

static int ce_capture_live_arm(uint32_t galaxy_ptr) {
    uint32_t con_list, con_array, con_count;
    uint32_t sector_rows[24];
    uint32_t sector_row_count = 0u;
    uint32_t index;
    ce_live_con_snapshot *new_cons;
    ce_live_sector_snapshot *new_sectors;
    ce_live_sector_snapshot *new_sector_rows = NULL;
    if (g_ce_live_snapshot_galaxy == galaxy_ptr && g_ce_live_cons != NULL) return 1;
    if (g_ce_live_snapshot_galaxy != 0u) {
        /* galaxy_ptr no longer matches what we captured before -- most
           likely the player loaded a different (or the same) save from
           the in-game menu without restarting Rangers.exe, which appears
           to allocate a fresh TGalaxy object rather than reusing the old
           one. A full process restart would have reset every one of these
           globals to zero fresh via DllMain, so this branch specifically
           means "same process, new galaxy" -- the old capture is now
           permanently stale (CEAdapterSetSystemSector/CEAdapterPortalReady
           both gate on an exact galaxy_ptr match, so leaving it as-is
           would silently and permanently break the portal/arm-switch
           feature for the rest of this process's life, which is exactly
           the "anchor stops working after reloading an earlier save" bug
           this fixes). Free the stale snapshot and fall through to
           recapture fresh for the new galaxy. There is no reliable way to
           know which arm should be "active" for a freshly loaded save --
           this mod's arm state is a pure runtime overlay, never persisted
           in the save file itself -- so default back to the old arm, same
           as a fresh process would start. Any in-flight portal state is
           tied to the old (now invalid) galaxy_ptr too and must be
           dropped for the same reason. */
        if (g_ce_live_cons != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_cons);
        if (g_ce_live_sectors != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_sectors);
        if (g_ce_live_sector_rows != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_sector_rows);
        if (g_ce_live_planets != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_planets);
        if (g_ce_live_stations != NULL) HeapFree(GetProcessHeap(), 0, g_ce_live_stations);
        g_ce_live_cons = NULL;
        g_ce_live_sectors = NULL;
        g_ce_live_sector_rows = NULL;
        g_ce_live_planets = NULL;
        g_ce_live_stations = NULL;
        g_ce_live_planet_count = 0u;
        g_ce_live_planet_capacity = 0u;
        g_ce_live_station_count = 0u;
        g_ce_live_station_capacity = 0u;
        g_ce_live_con_count = 0u;
        g_ce_live_mapped_system_count = 0u;
        g_ce_live_sector_count = 0u;
        g_ce_live_sector_row_count = 0u;
        g_ce_live_snapshot_galaxy = 0u;
        InterlockedExchange(&g_ce_active_arm, 0);
        InterlockedExchange(&g_ce_portal_status, 0);
        InterlockedExchange(&g_ce_portal_hole_id, 0);
        InterlockedExchange(&g_ce_portal_galaxy_ptr, 0);
        InterlockedExchange(&g_ce_live_switch_lock, 0);
    }
    char diagnostic[256];
    int diagnostic_size;
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x38u), 4u, 0)) {
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"capture-failed\",\"stage\":\"galaxy\"}\r\n", 47u);
        return 0;
    }
    con_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    if (!ce_read_plain_list(con_list, &con_array, &con_count)) {
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"capture-failed\",\"stage\":\"systems\"}\r\n", 48u);
        return 0;
    }
    /* ce_find_live_sector_rows used to run here to refresh UI row labels
       for already-open sectors. It is a synchronous full-address-space
       VirtualQuery/byte scan (lpMinimumApplicationAddress through
       lpMaximumApplicationAddress), and it ran exactly once ever per
       galaxy (guarded above) -- squarely on the same first Turn-code
       invocation that reliably crashed the game in TGalaxy.NextDay on a
       brand new save, on every attempt, regardless of which thread/hook
       variant was tried. Its own comment already says it is not
       authoritative (StarToCon() supplies every real TConstellation
       below), so it is cosmetic, not required -- not worth an expensive
       synchronous scan from code that appears to run nested inside
       NextDay's call stack. Skipped entirely; sector_row_count stays 0. */
    sector_row_count = 0u;
    new_cons = (ce_live_con_snapshot *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, con_count * sizeof(*g_ce_live_cons));
    new_sectors = (ce_live_sector_snapshot *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, 24u * sizeof(*g_ce_live_sectors));
    if (sector_row_count != 0u) {
        new_sector_rows = (ce_live_sector_snapshot *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            sector_row_count * sizeof(*g_ce_live_sector_rows));
    }
    if (new_cons == NULL || new_sectors == NULL ||
            (sector_row_count != 0u && new_sector_rows == NULL)) {
        if (new_cons != NULL) HeapFree(GetProcessHeap(), 0, new_cons);
        if (new_sectors != NULL) HeapFree(GetProcessHeap(), 0, new_sectors);
        if (new_sector_rows != NULL) HeapFree(GetProcessHeap(), 0, new_sector_rows);
        return 0;
    }
    for (index = 0; index < con_count; ++index) {
        uint32_t object = *(const uint32_t *)(uintptr_t)(con_array + index * 4u);
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x1cu, 1)) goto capture_fail;
        new_cons[index].object = object;
        new_cons[index].old_name = *(const uint32_t *)(uintptr_t)(object + 0x10u);
        memcpy(&new_cons[index].old_x,
            (const void *)(uintptr_t)(object + 0x14u), sizeof(float));
        memcpy(&new_cons[index].old_y,
            (const void *)(uintptr_t)(object + 0x18u), sizeof(float));
        new_cons[index].second_name = ce_make_immortal_unicode(
            g_ce_second_system_names[index % (sizeof(g_ce_second_system_names) /
                sizeof(g_ce_second_system_names[0]))]);
        if (new_cons[index].second_name == 0u) goto capture_fail;
        new_cons[index].sector_object = 0u;
    }
    for (index = 0; index < sector_row_count; ++index) {
        uint32_t object = sector_rows[index];
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x1cu, 1)) goto capture_fail;
        new_sector_rows[index].object = object;
        new_sector_rows[index].old_name = *(const uint32_t *)(uintptr_t)(object + 0x18u);
        new_sector_rows[index].second_name_index = UINT32_MAX;
    }
    g_ce_live_cons = new_cons;
    g_ce_live_sectors = new_sectors;
    g_ce_live_sector_rows = new_sector_rows;
    g_ce_live_con_count = con_count;
    g_ce_live_mapped_system_count = 0u;
    g_ce_live_sector_count = 0u;
    g_ce_live_sector_row_count = sector_row_count;
    g_ce_live_snapshot_galaxy = galaxy_ptr;
    diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"captured\",\"systems\":%u,\"sector_rows\":%u}\r\n",
        con_count, sector_row_count);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    return 1;

capture_fail:
    for (index = 0u; index < con_count; ++index) {
        if (new_cons[index].second_name != 0u) {
            HeapFree(GetProcessHeap(), 0,
                (void *)(uintptr_t)(new_cons[index].second_name - 8u));
        }
    }
    HeapFree(GetProcessHeap(), 0, new_cons);
    HeapFree(GetProcessHeap(), 0, new_sectors);
    if (new_sector_rows != NULL) HeapFree(GetProcessHeap(), 0, new_sector_rows);
    {
        const char *failure =
            "{\"status\":\"capture-failed\",\"stage\":\"snapshot\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", failure, strlen(failure));
    }
    return 0;
}

static uint32_t ce_live_sector_index(uint32_t object) {
    uint32_t index;
    for (index = 0u; index < g_ce_live_sector_count; ++index) {
        if (g_ce_live_sectors[index].object == object) return index;
    }
    return UINT32_MAX;
}

static int ce_live_names_equal(uint32_t left, uint32_t right) {
    const wchar_t *left_text, *right_text;
    uint32_t left_count, right_count;
    if (left == right) return 1;
    if (!ce_live_read_wide_string(left, &left_text, &left_count) ||
            !ce_live_read_wide_string(right, &right_text, &right_count) ||
            left_count != right_count) return 0;
    return memcmp(left_text, right_text, left_count * sizeof(wchar_t)) == 0;
}

static volatile LONG g_ce_pending_sector_spawn_arm = -1;
static volatile LONG g_ce_camera_recenter_ticks = 0;
static volatile LONG g_ce_pending_arrival_direction = -1;
static volatile LONG g_ce_pending_arrival_ticks = 0;
static volatile LONG g_ce_origin_destination_index = -1;
static volatile LONG g_ce_ab_test_ordered = 0;

/* Remembers the ARRAY INDEX (not the star pointer itself -- that belongs to
   the OLD galaxy and is meaningless once we swap) of the destination star
   chosen at hole-creation time, in CE_InterarmTransit.Lang.txt /
   CE_MapSmoke.Main.txt's own "farthest, different sector, visible" scan.
   The arrival block later reads GalaxyStar() at this SAME index from
   whichever galaxy just became active (Second Home), instead of picking
   its own "nearest to wherever the native hole-exit landed" star and then
   overriding CoordX/CoordY to match it. That override was the actual
   cause of the visual "double jump" complaint: the native hole-exit
   sequence already correctly repositions the ship (both FCurStar and
   FPos) before Turn-code ever sees ShipInHole() go false, and forcibly
   moving it again undid that. Reusing the SAME index at least ties the
   post-swap star to the SAME "how interesting/far" choice the player's
   hole was already built around, without touching position at all. */
uint32_t CE_CALL CEAdapterSetOriginDestinationIndex(uint32_t index) {
    InterlockedExchange(&g_ce_origin_destination_index, (LONG)index);
    return 1u;
}

uint32_t CE_CALL CEAdapterConsumeOriginDestinationIndex(void) {
    return (uint32_t)InterlockedExchange(&g_ce_origin_destination_index, -1);
}

/* Confirmed live via turn-heartbeat.jsonl: Turn-code stops ticking
   altogether right after a swap completes -- on ENTER this time, not just
   return, so it isn't specific to direction. The one piece of RScript that
   runs unconditionally, in the SAME tick as the swap, touching the
   freshly-(re)activated galaxy's own data is the arrival block's sector
   reveal (GalaxySectors()/ConNear() loop). Deferring that whole block by a
   few ticks -- instead of running it in the very same tick the swap
   happened -- gives the engine a chance to settle before anything touches
   Galaxy.FConstellation again, the same "retry a few ticks later instead
   of doing it eagerly" shape already used for the camera recenter above.

   Raised from 3 to 90 earlier this session on the theory that the native
   "fly out of the hole" sequence needed several real seconds to finish
   playing before it was safe to reposition the ship -- but that theory was
   never actually measured, only inferred by analogy with the archived
   cosmetic system (which never needed ANY delay, because it never swapped
   galaxies or touched ship position at all -- there was nothing for a
   delay to protect there, so its lack of one proves nothing about how long
   the native sequence itself runs). Lowering to 5 was tried and made things
   WORSE: turn-heartbeat logging showed it froze on the very same tick as
   the swap (before a 5-tick countdown could even elapse), the ship ended
   up stranded at raw native-exit coordinates with no sector opened, and
   the process eventually crashed. Reverted to the known-stable 90 -- the
   actual fix for the missing exit animation turned out to be a different
   variable: the ENTER arrival block (CE_MapSmoke.rson) was overwriting
   CoordX/CoordY after TransferShip, which the RETURN arrival block never
   does and which is confirmed live (by the player) to still show the
   native exit animation correctly. Isolate that change on its own instead
   of also re-touching this delay. */
uint32_t CE_CALL CEAdapterArmPendingArrival(uint32_t entering) {
    InterlockedExchange(&g_ce_pending_arrival_direction, entering != 0u ? 1 : 0);
    /* Was 90, inherited from the raw pointer-swap era where the engine
       needed time to settle. The transition now goes through the engine's
       own LoadGame, and the traveller lands DOCKED at the new game's
       station, where Turn-code barely runs -- so a 90-tick countdown was
       effectively waiting for 90 day-skips and the whole arrival block
       (hole, greeting, stat transfer) never fired. */
    /* Zero, not a countdown. Measured: after the load the traveller stands
       DOCKED at the target world's station, and Turn-code ticks once and
       then waits for a day skip that never comes -- so even a 2-tick delay
       parked the arrival forever. The delay existed for the raw
       pointer-swap era, where the engine needed to settle before anything
       touched the newly active galaxy; a completed LoadGame has already
       settled, so the first Turn after it is the right moment. */
    /* One, not zero. Arming and polling both happen inside the SAME Turn
       invocation -- CEAdapterCompleteRegisteredPortal runs near the top of
       the turn code and CEAdapterConsumePendingArrival a few lines below --
       so a zero countdown fired the arrival immediately, while the old arm
       was still live and FormChange('GameLoad') had not taken effect yet.
       Everything then landed in the world that was about to be discarded:
       the captain's money and skills were written to the departing player
       and the arrival hole was opened in the departing galaxy. One tick
       lets the load happen first, and Turn-code runs again right after it. */
    InterlockedExchange(&g_ce_pending_arrival_ticks, 1);
    return 1u;
}

/* Returns 0 (nothing ready yet, keep waiting or nothing pending), 1 (ready,
   was entering), or 2 (ready, was returning) -- consumes the pending state
   exactly once, on the tick the countdown reaches zero. */
uint32_t CE_CALL CEAdapterConsumePendingArrival(void) {
    LONG direction = InterlockedCompareExchange(&g_ce_pending_arrival_direction, 0, 0);
    LONG remaining;
    if (direction < 0) return 0u;
    /* This poll is reached from the target save's live Turn graph.  Publish
       the target arm here, after LoadGame, never while the target Galaxy is
       still being reconstructed. */
    InterlockedExchange(&g_ce_active_arm, direction == 1 ? 1 : 0);
    remaining = InterlockedCompareExchange(&g_ce_pending_arrival_ticks, 0, 0);
    if (remaining > 0) {
        InterlockedExchange(&g_ce_pending_arrival_ticks, remaining - 1);
        return 0u;
    }
    InterlockedExchange(&g_ce_pending_arrival_direction, -1);
    return direction == 1 ? 1u : 2u;
}

/* StarMapCenterView is a documented no-op whenever the StarMap form isn't
   the one currently displayed (GForm[GFormCur]<>GFormStarMap) -- calling it
   exactly once, at the moment a portal transition completes, only works if
   the player happens to already be looking at the galaxy map at that exact
   instant. Confirmed live: they usually aren't (just flew through a hole,
   likely on the Ship panel, or watching the arcade encounter), so the
   one-shot call silently did nothing and the camera stayed wherever it
   last was. Turn-code polls this counter every tick instead and keeps
   retrying StarMapCenterView for a while after arrival, so whichever tick
   the player actually has the map open on, the recenter goes through.
   40 ticks (a few seconds) turned out to not be nearly enough -- live
   testing showed the camera still stuck on the pre-swap view well after
   an arcade encounter plus however long it took to switch back to the map
   screen. Raised by an order of magnitude; still just a retry budget, not
   a real time bound, and StarMapCenterView itself stays a cheap no-op on
   every tick it doesn't apply. */
uint32_t CE_CALL CEAdapterArmCameraRecenter(void) {
    InterlockedExchange(&g_ce_camera_recenter_ticks, 3000);
    return 1u;
}

uint32_t CE_CALL CEAdapterConsumeCameraRecenterPending(void) {
    LONG remaining = InterlockedCompareExchange(&g_ce_camera_recenter_ticks, 0, 0);
    if (remaining <= 0) return 0u;
    InterlockedExchange(&g_ce_camera_recenter_ticks, remaining - 1);
    return 1u;
}

/* Called right after a tick where the recenter (with its ring-animation
   flourish, see CE_MapSmoke.rson) actually landed -- i.e. Turn-code
   confirmed CurrentForm()=='StarMap' that same tick. Without this, the
   retry budget would keep counting down and re-fire StarMapCenterView
   (ring animation included) on every remaining tick for as long as the
   player stays on the map, replaying the ring over and over instead of
   once. */
uint32_t CE_CALL CEAdapterClearCameraRecenterPending(void) {
    InterlockedExchange(&g_ce_camera_recenter_ticks, 0);
    return 1u;
}

/* Purely diagnostic, temporary: answers "does Turn-code keep ticking while
   an arcade combat encounter is on screen" empirically instead of by
   guessing, since that answer decides whether the portal swap can move to
   hole-entry (so the swap happens behind the arcade's own native loading
   transitions) or has to stay on hole-exit as now. Deliberately does not
   gate on anything -- call every tick and just compare timestamps in the
   log against when the player was actually fighting. */
uint32_t CE_CALL CEAdapterLogTurnHeartbeat(uint32_t turn, uint32_t ship_in_hole) {
    char payload[96];
    int size = snprintf(payload, sizeof(payload),
        "{\"turn\":%lu,\"ship_in_hole\":%lu,\"tick\":%lu}\r\n",
        (unsigned long)turn, (unsigned long)ship_in_hole,
        (unsigned long)GetTickCount());
    if (size > 0) ce_write_text_marker("turn-heartbeat.jsonl", payload, (size_t)size);
    return 1u;
}

/* Purely diagnostic, temporary: the player has now reported the arrival
   point landing "on the far side of the map" with its sector never
   opened, on more than one build in a row (including one where the
   ship's coordinates were no longer being force-set at all) -- meaning
   this needs actual numbers instead of another guess. Logs the raw
   native hole-exit position, the star CE_MapSmoke.rson's own "nearest
   candidate" scan picked, the galaxy's star/sector counts, and how many
   neighbour sectors the reveal loop actually opened, all from the same
   tick, so a single test flight settles what the arrival logic is really
   doing instead of inferring it from on-screen symptoms alone. */
uint32_t CE_CALL CEAdapterLogArrivalDiagnostics(
    uint32_t native_x, uint32_t native_y,
    uint32_t star_x, uint32_t star_y,
    uint32_t galaxy_stars, uint32_t galaxy_sectors,
    uint32_t arrival_sector, uint32_t opened_count
) {
    char payload[224];
    int size = snprintf(payload, sizeof(payload),
        "{\"native_x\":%ld,\"native_y\":%ld,\"star_x\":%ld,\"star_y\":%ld,"
        "\"galaxy_stars\":%lu,\"galaxy_sectors\":%lu,\"arrival_sector\":%lu,"
        "\"opened_count\":%lu}\r\n",
        (long)(int32_t)native_x, (long)(int32_t)native_y,
        (long)(int32_t)star_x, (long)(int32_t)star_y,
        (unsigned long)galaxy_stars, (unsigned long)galaxy_sectors,
        (unsigned long)arrival_sector, (unsigned long)opened_count);
    if (size > 0) ce_write_text_marker("arrival-diagnostics.jsonl", payload, (size_t)size);
    return 1u;
}

/* Purely diagnostic, temporary: isolated test of the StartAB-as-loading-cover
   idea. gab_status is the calling script's own GABStatus local (read and
   passed in by RScript, since it isn't reachable from native code) --
   logging it alongside ship_in_hole and a native tick timestamp lets us see,
   after a single test flight, exactly how long a zero-ship arcade battle
   takes to resolve (GABStatus 0 -> 1 -> 2) instead of guessing. */
uint32_t CE_CALL CEAdapterLogABTestEvent(uint32_t turn, uint32_t ship_in_hole, uint32_t gab_status) {
    char payload[112];
    int size = snprintf(payload, sizeof(payload),
        "{\"turn\":%lu,\"ship_in_hole\":%lu,\"gab_status\":%lu,\"tick\":%lu}\r\n",
        (unsigned long)turn, (unsigned long)ship_in_hole, (unsigned long)gab_status,
        (unsigned long)GetTickCount());
    if (size > 0) ce_write_text_marker("ab-test.jsonl", payload, (size_t)size);
    return 1u;
}

/* GABStatus resets to 0 within the SAME turn its own completion pulse
   (status 2) is observed, while ShipInHole() can easily still read true for
   several more ticks after that (native hole-exit is not instantaneous) --
   a plain "ShipInHole() && GABStatus==0" turncode check re-fires StartAB a
   second time before the ship has actually left the hole. This latch tracks
   "already ordered a battle for the CURRENT hole visit" independently of
   GABStatus's own churn; RScript clears it the moment ShipInHole() reads
   false again. */
uint32_t CE_CALL CEAdapterIsABTestOrdered(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_ab_test_ordered, 0, 0);
}

uint32_t CE_CALL CEAdapterArmABTestOrdered(void) {
    InterlockedExchange(&g_ce_ab_test_ordered, 1);
    return 1u;
}

uint32_t CE_CALL CEAdapterClearABTestOrdered(void) {
    InterlockedExchange(&g_ce_ab_test_ordered, 0);
    return 1u;
}

/* ce_apply_live_arm (below) sets this same flag, but it also does a lot of
   cosmetic-scheme-specific work (swapping star/planet/station names back
   and forth on a SHARED galaxy's own objects) that has no meaning for the
   genuinely separate second TGalaxy this session's swap now uses -- that
   renaming is handled directly by CEAdapterRenameSecondGalaxySystems
   instead. ce_sector_name_helper.exe itself only ever reads/rewrites
   whichever galaxy's sector labels are CURRENTLY ON SCREEN (found by
   scanning for the always-present vanilla needle string, not tied to a
   specific TGalaxy's own memory), so it works unmodified for either
   scheme -- this export just requests the same background spawn, without
   any of the unrelated shared-galaxy bookkeeping. */
uint32_t CE_CALL CEAdapterRequestSectorLabelSpawn(uint32_t second_home) {
    InterlockedExchange(&g_ce_pending_sector_spawn_arm, second_home != 0u ? 1 : 0);
    return 1u;
}

/* Launches tools/native/ce_sector_name_helper.exe (built to the same
   directory as this DLL, see build-engine-adapter.ps1) as a completely
   separate OS process, passing our own PID and the target arm, so it can
   rewrite the 20 Constellations.Name "row" objects via its own
   ReadProcessMemory/WriteProcessMemory calls -- see the comment above
   g_ce_second_planet_names for why this has to live outside this process.

   IMPORTANT: this must never be called directly from ce_apply_live_arm (or
   anything else reachable from Turn-code) -- an early version did exactly
   that, reasoning that CreateProcess "returns almost immediately, so it
   cannot reintroduce the loading-stall bug that a blocking Sleep() caused
   right here previously." That reasoning only ruled out the BLOCKING
   variant of the hazard; a live crash on portal entry (matching the exact
   TGalaxy.NextDay crash class already fought earlier in this project --
   see the DllMain/ce_hook_thread_proc comments) showed that the hazard is
   the heavy OS call ITSELF happening on a stack nested inside NextDay, not
   specifically blocking. CreateThread was already known-unsafe there;
   CreateProcess apparently is too. This function must only ever be called
   from ce_sector_spawn_thread_proc, a dedicated thread started once from
   DllMain (never from Turn-code), which polls g_ce_pending_sector_spawn_arm
   instead. ce_apply_live_arm only ever sets that flag (a plain interlocked
   write, no OS call, safe from any call stack) -- see the bottom of that
   function. */
static void ce_spawn_sector_name_helper(int second_home) {
    char module_path[MAX_PATH];
    char command_line[MAX_PATH + 64];
    char *last_slash;
    DWORD length;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    SECURITY_ATTRIBUTES inheritable;
    HANDLE log_file;

    length = GetModuleFileNameA(g_ce_adapter_instance, module_path, sizeof(module_path));
    if (length == 0u || length >= sizeof(module_path)) return;
    last_slash = strrchr(module_path, '\\');
    if (last_slash == NULL) return;
    last_slash[1] = '\0';
    if (snprintf(command_line, sizeof(command_line), "\"%sce_sector_name_helper.exe\" %lu %s",
            module_path, (unsigned long)GetCurrentProcessId(),
            second_home ? "second" : "old") < 0) return;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));
    /* This tool's own wprintf output (found/changed/failed counts, or the
       "could not confirm anchor" failure) was previously discarded
       (CREATE_NO_WINDOW with no stdio redirection at all) -- meaning a
       silent revert failure on return (sector names still showing Second
       Home's) was completely invisible. Redirect both streams to a shared,
       append-mode log instead so each invocation's outcome is on record. */
    ZeroMemory(&inheritable, sizeof(inheritable));
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    CreateDirectoryA("C:\\ce_debug", NULL);
    log_file = CreateFileA("C:\\ce_debug\\sector-name-helper.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file != INVALID_HANDLE_VALUE) {
        startup_info.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.hStdOutput = log_file;
        startup_info.hStdError = log_file;
    }
    if (CreateProcessA(NULL, command_line, NULL, NULL, log_file != INVALID_HANDLE_VALUE,
            CREATE_NO_WINDOW, NULL, NULL, &startup_info, &process_info)) {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
    }
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
}

/* Runs forever on its own thread (started once from DllMain, see the
   comment there), completely decoupled from Turn-code/NextDay. Polling
   interval is not latency-sensitive: the sector rename is already a
   one-time visual settle after a portal transition, a few hundred
   milliseconds of extra lag here is unnoticeable. */
static DWORD WINAPI ce_sector_spawn_thread_proc(LPVOID unused) {
    (void)unused;
    for (;;) {
        LONG requested = InterlockedExchange(&g_ce_pending_sector_spawn_arm, -1);
        if (requested >= 0) ce_spawn_sector_name_helper(requested != 0);
        Sleep(75);
    }
}

/* No longer called (CEAdapterCompleteRegisteredPortal now drives the real
   swap instead -- see its own comment) -- kept, not deleted, matching this
   file's existing precedent for archived-but-still-buildable dead code
   (e.g. CEAdapterSetRememberedShipStar/GetRememberedShipStar). __used__
   only to keep -Wunused-function -Werror quiet; unused-warnings for the
   helpers it alone calls would cascade otherwise. */
__attribute__((used)) static void ce_apply_live_arm(int second_home) {
    uint32_t index;
    uint32_t seed = (uint32_t)InterlockedCompareExchange(&g_ce_live_seed, 0, 0);
    float sector_x[24] = {0};
    float sector_y[24] = {0};
    uint32_t sector_members[24] = {0};
    char report[256];
    int report_size;
    if (g_ce_live_cons == NULL || g_ce_live_sectors == NULL ||
            g_ce_live_sector_count == 0u) return;
    qsort(g_ce_live_sectors, g_ce_live_sector_count,
        sizeof(g_ce_live_sectors[0]), ce_compare_live_sector_objects);
    for (index = 0; index < g_ce_live_con_count; ++index) {
        uint32_t sector = ce_live_sector_index(g_ce_live_cons[index].sector_object);
        if (sector < g_ce_live_sector_count) {
            sector_x[sector] += g_ce_live_cons[index].old_x;
            sector_y[sector] += g_ce_live_cons[index].old_y;
            ++sector_members[sector];
        }
    }
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        if (sector_members[index] != 0u) {
            sector_x[index] /= (float)sector_members[index];
            sector_y[index] /= (float)sector_members[index];
        }
    }
    /* A galaxy-center point reflection of each sector's star cluster was
       tried here (mirroring stars to the opposite side of the map) and
       reverted: sector BOUNDARY POLYGONS and their name-label anchor are
       drawn from the sector object's own separately-stored shape data, not
       recomputed from member star positions, so moving only the stars left
       them floating outside/across their sector's unmoved outline -- a
       visibly broken map, reported by the user as "crooked" and matching
       an earlier-seen bug class. Fixing this for real would mean also
       relocating the sector's own polygon data, which needs its own live
       dump investigation (same category as the name-offset hunts, not
       attempted yet) since the sector object's exact shape fields are
       still unknown. Back to jittering within each star's existing sector
       cluster only -- safe, but does not relocate sectors on the map. */
    for (index = 0; index < g_ce_live_con_count; ++index) {
        ce_live_con_snapshot *item = &g_ce_live_cons[index];
        float x = item->old_x;
        float y = item->old_y;
        if (second_home) {
            uint32_t id = *(const uint32_t *)(uintptr_t)(item->object + 4u);
            uint32_t hx = ce_mix32(seed ^ id ^ (index * 0x9e3779b9u));
            uint32_t hy = ce_mix32((seed + 0x85ebca6bu) ^ id ^ (index * 0xc2b2ae35u));
            uint32_t sector = ce_live_sector_index(item->sector_object);
            if (sector < g_ce_live_sector_count && sector_members[sector] != 0u) {
                float dx = item->old_x - sector_x[sector];
                float dy = item->old_y - sector_y[sector];
                float scale = 0.66f + ((float)(hx & 0xffu) / 255.0f) * 0.12f;
                float skew = ((float)((hy >> 8u) & 0xffu) / 255.0f - 0.5f) * 0.16f;
                x = sector_x[sector] + dx * scale - dy * skew;
                y = sector_y[sector] + dy * scale + dx * skew;
            }
        }
        *(uint32_t *)(uintptr_t)(item->object + 0x10u) =
            second_home ? item->second_name : item->old_name;
        memcpy((void *)(uintptr_t)(item->object + 0x14u), &x, sizeof(x));
        memcpy((void *)(uintptr_t)(item->object + 0x18u), &y, sizeof(y));
    }
    /* Sector NAME rewriting never touches this object at all -- see the
       comment above g_ce_second_planet_names for why (+0x0c here is a
       coordinate, not a name pointer; the real label lives in a separate
       config-tree "row" object found and rewritten by an external helper
       process, spawned below via ce_spawn_sector_name_helper). This loop
       only ever runs g_ce_live_sector_row_count times, which stays 0 --
       ce_find_live_sector_rows (the in-process scan that would have
       populated it) is permanently disabled after correlating with a
       crash -- so it is dead code kept only so the row-snapshot capture
       machinery above compiles; it can be removed outright once nothing
       else references g_ce_live_sector_rows. */
    for (index = 0; index < g_ce_live_sector_row_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sector_rows[index];
        uint32_t sector_index;
        uint32_t second_name = 0u;
        for (sector_index = 0u; sector_index < g_ce_live_sector_count; ++sector_index) {
            if (ce_live_names_equal(
                    item->old_name, g_ce_live_sectors[sector_index].old_name)) {
                second_name = g_ce_live_sectors[sector_index].second_name;
                break;
            }
        }
        if (second_name == 0u && index < g_ce_live_sector_count)
            second_name = g_ce_live_sectors[index].second_name;
        *(uint32_t *)(uintptr_t)(item->object + 0x18u) =
            second_home && second_name != 0u ? second_name : item->old_name;
    }
    /* Sector visibility ("explored") flag: bit 0x100 of the dword at
       +0x08, confirmed by a live before/after dump diff (see
       CEAdapterProbeSectorFlagBefore/After) -- every other byte of the
       object was identical before and after SectorVisible(sector,1).
       Unlike SectorVisible() itself, which the documented RScript API can
       only use to OPEN a sector, writing the bit directly can also clear
       it. Entering Second Home closes every sector captured so far (the
       Turn-code that runs right after this opens the arrival sector and
       a couple of neighbors via the normal RScript SectorVisible() call);
       returning to the old arm restores each sector's exact original
       state, captured once in CEAdapterSetSystemSector. The low byte of
       the same dword carries unrelated state and is preserved as-is. */
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sectors[index];
        uint32_t *flag_ptr = (uint32_t *)(uintptr_t)(item->object + 0x08u);
        uint32_t current = *flag_ptr;
        uint32_t want_bit = second_home ? 0u : item->old_visible;
        *flag_ptr = (current & ~0x100u) | want_bit;
    }
    /* Sector name: +0x04 is the 1-based row number into the
       Constellations.Name Lang table (see old_sector_number capture
       comment above). First attempt pointed this at rows 21-44 (this
       mod's own Second Home names, added to that table in
       CE_MapSmoke.Lang.txt) and it had no visible effect in-game --
       sectors kept their vanilla labels while stars/planets/stations
       (same swap-and-restore pattern, different offsets) all renamed
       correctly. Most likely explanation: the map's sector-label drawing
       is native code that resolved Constellations.Name into a fixed-size
       internal table ONCE at startup, sized to the base game's own 20
       rows, and never re-queries the merged Lang.dat by index -- so row
       21+ may simply be unreachable by this mechanism, unlike CT(),
       which RScript queries dynamically by string key. Testing that
       narrower hypothesis before concluding it: permute within the
       KNOWN-valid 1-20 range instead of extending past it. If a sector
       shows a DIFFERENT vanilla name after this, +0x04 does control the
       label and only the >20 extension is the problem; if nothing
       changes even within 1-20, this field is not what draws the label
       at all. Offset +7 (mod 20) is arbitrary, just non-zero. */
    for (index = 0; index < g_ce_live_sector_count; ++index) {
        ce_live_sector_snapshot *item = &g_ce_live_sectors[index];
        uint32_t new_number = ((item->old_sector_number - 1u + 7u) % 20u) + 1u;
        *(uint32_t *)(uintptr_t)(item->object + 0x04u) =
            second_home ? new_number : item->old_sector_number;
    }
    /* Planet (+0x14) and station (+0x08) name-pointer offsets confirmed by
       CEAdapterDumpNamedObject live dumps -- same swap-and-restore shape as
       the star loop above, just per-class offsets. */
    for (index = 0; index < g_ce_live_planet_count; ++index) {
        ce_live_named_snapshot *item = &g_ce_live_planets[index];
        *(uint32_t *)(uintptr_t)(item->object + 0x14u) =
            second_home ? item->second_name : item->old_name;
    }
    for (index = 0; index < g_ce_live_station_count; ++index) {
        ce_live_named_snapshot *item = &g_ce_live_stations[index];
        *(uint32_t *)(uintptr_t)(item->object + 0x08u) =
            second_home ? item->second_name : item->old_name;
    }
    InterlockedExchange(&g_ce_active_arm, second_home ? 1 : 0);
    InterlockedExchange(&g_ce_pending_sector_spawn_arm, second_home ? 1 : 0);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"switched\",\"abi\":%u,\"arm\":\"%s\","
        "\"systems\":%u,\"sectors\":%u,\"planets\":%u,\"stations\":%u}\r\n",
        CEAdapterAbiVersion(), second_home ? "SECOND_HOME" : "OLD_ARM",
        g_ce_live_con_count, g_ce_live_sector_count,
        g_ce_live_planet_count, g_ce_live_station_count);
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
}

/* F8 is a vanilla quick-save key.  Consume it and forward an otherwise unused
   F24 key to the script UI, so opening the inter-arm portal never also opens
   the vanilla "quick save is absent" dialog. */
/* This used to be a thread-specific WH_KEYBOARD hook, which only ever sees
   keys that pass through GetMessage/PeekMessage on the hooked thread's
   queue. Live testing showed vanilla's own F8 quicksave still fired every
   time regardless -- this engine almost certainly reads its hotkeys from
   polled raw key state (DirectInput-style), not from WM_KEYDOWN messages,
   so swallowing the message never stopped it. WH_KEYBOARD_LL sits at the
   OS-wide low-level input stage, ahead of that polled state entirely:
   returning nonzero here drops the keystroke system-wide before any
   consumer (message queue or polling) ever observes it. */
static void ce_post_virtual_key(HWND window, UINT key) {
    PostMessageW(window, WM_KEYDOWN, key, 1L);
    PostMessageW(window, WM_KEYUP, key, 0xc0000001L);
}

/* The Ctrl+Shift+ELTAN test cheat used to be detected here too, but that
   was solving a problem RScript already solves natively: OnKey exposes
   KEY and KEYMOD (Shift=1, Ctrl=2, Alt=4) straight from the engine, with
   no vanilla binding standing in the way for a plain letter combo (unlike
   F8, which vanilla's own quickload claims before any script ever sees
   it). The cheat now lives entirely in CE_MapSmoke.Main.txt using those,
   which also sidesteps needing this hook to run at all for it. */
static LRESULT CALLBACK ce_portal_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *info = (const KBDLLHOOKSTRUCT *)lparam;
        if (info->vkCode == VK_F8) {
            HWND game_window = GetForegroundWindow();
            DWORD game_pid = 0;
            int is_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
            int is_up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
            GetWindowThreadProcessId(game_window, &game_pid);
            if (game_pid == GetCurrentProcessId()) {
                if (is_down) {
                    if (InterlockedExchange(&g_ce_f8_down, 1) == 0)
                        ce_post_virtual_key(game_window, VK_F24);
                } else if (is_up) {
                    InterlockedExchange(&g_ce_f8_down, 0);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(g_ce_keyboard_hook, code, wparam, lparam);
}

/* Per Microsoft's own docs for WH_KEYBOARD_LL: "This hook is called in the
   context of the thread that installed it ... therefore the thread that
   installed the hook must have a message loop." A thread that only pumps
   messages and installs the hook is required; two earlier variants of
   that thread both crashed the game on turn 0 of a brand new game with a
   native exception in TGalaxy.NextDay:
     1) spawning the thread from CEAdapterInstallLiveArmSwitch and
        blocking the caller on an event until the hook was confirmed;
     2) spawning it fire-and-forget from that same call, without blocking.
   Turn-script code appears to run nested inside NextDay's own call stack,
   so apparently even just calling CreateThread from anywhere reachable
   from it is unsafe. This thread is now started from DllMain instead (see
   the forward declaration above), which fires once at process attach,
   before any galaxy or Turn processing exists -- no connection to
   NextDay's call stack at all. */
static DWORD WINAPI ce_hook_thread_proc(LPVOID unused) {
    MSG msg;
    (void)unused;
    if (GetFileAttributesA("C:\\ce_debug\\trace-game-exceptions.flag") !=
            INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA("C:\\ce_debug", NULL);
        g_ce_exception_trace_file = CreateFileA(
            "C:\\ce_debug\\game-exceptions.bin", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_ce_exception_trace_file != INVALID_HANDLE_VALUE) {
            ce_ensure_veh_installed();
            ce_install_package_file_open_trace_hook();
            ce_install_process_exit_trace_hooks();
            ce_install_unhandled_exception_trace_hook();
        }
    }
    /* Install before the player can start a party.  Unlike all live-arm
       experiments this detour only runs inside the engine's own native
       new-game worker, never from a Turn/NextDay callback. */
    ce_install_dual_newgame_hook();
    /* A thread has no message queue until it calls a USER32 function that
       needs one; force its creation before installing the hook. */
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    g_ce_keyboard_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL, ce_portal_keyboard_proc, g_ce_adapter_instance, 0);
    if (g_ce_keyboard_hook != NULL) {
        const char *ready = "{\"status\":\"ready\",\"mode\":\"manual-portal\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", ready, strlen(ready));
    }
    if (g_ce_keyboard_hook == NULL) return 1;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

/* The F8 portal only works if VK_F8 is remapped to VK_F24 by the keyboard
   hook installed from DllMain (see ce_hook_thread_proc/ce_portal_keyboard_proc
   above): the script's OnKey handler listens for F24, not F8, precisely so
   the vanilla quicksave binding on F8 stays suppressed. This function used
   to install the hook itself (lazily, the first time it saw galaxy_ptr
   resolve), but every variant of spawning a thread from here -- reachable
   from the Turn script, itself apparently nested inside TGalaxy.NextDay --
   crashed the game on turn 0 of a new game. It now only checks whether the
   DllMain-started thread has finished installing the hook yet. */
uint32_t CE_CALL CEAdapterInstallLiveArmSwitch(uint32_t galaxy_ptr, uint32_t seed) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    InterlockedExchange(&g_ce_live_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_live_seed, (LONG)seed);
    if (g_ce_keyboard_hook == NULL) return 0;
    return ce_capture_live_arm(galaxy_ptr);
}

static volatile LONG g_ce_ssc_logged = 0;
static volatile LONG g_ce_ssc_dumped = 0;

/* One-shot raw dump of the object StarToCon() actually returns, so the
   +0x0c name-field offset (inherited from the removed VMT-scan code,
   which targeted a different discovery path) can be verified or
   corrected against real bytes instead of guessed again. */
static void ce_dump_sector_object(uint32_t sector_ptr) {
    char diagnostic[4096];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"sector-object-dump\",\"sector_ptr\":%u,\"words\":[", sector_ptr);
    if (written < 0) return;
    used = (size_t)written;
    for (offset = 0u; offset < 0x140u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(sector_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(sector_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
}

uint32_t CE_CALL CEAdapterSetSystemSector(
        uint32_t galaxy_ptr, uint32_t system_index, uint32_t sector_ptr) {
    uint32_t sector_index;
    const char *reason = "ok";
    uint32_t result = 1u;

    /* ce_dump_sector_object is a cheap, bounded 0x140-byte read -- safe to
       run inline here. The name-string memory scan is NOT: it is the same
       expensive category of full-address-space walk as the removed
       ce_find_live_sector_rows, which reproduced a real TGalaxy.NextDay
       crash when run synchronously from this Turn-code-reachable path.
       It is started as a background thread from DllMain instead (see
       forward declaration/spawn there), completely decoupled from any
       Turn-code timing. */
    if (sector_ptr != 0u && InterlockedCompareExchange(&g_ce_ssc_dumped, 1, 0) == 0) {
        ce_dump_sector_object(sector_ptr);
    }

    if (g_ce_live_snapshot_galaxy != galaxy_ptr) { reason = "galaxy-mismatch"; result = 0u; }
    else if (g_ce_live_cons == NULL || g_ce_live_sectors == NULL) { reason = "not-captured"; result = 0u; }
    else if (system_index >= g_ce_live_con_count) { reason = "bad-index"; result = 0u; }
    else if (sector_ptr == 0u) { reason = "sector-ptr-zero"; result = 0u; }

    if (result != 0u) {
        sector_index = ce_live_sector_index(sector_ptr);
        if (sector_index == UINT32_MAX) {
            /* The dumped object layout (VMT@+0x00, floats at +0x0c/+0x10,
               a 0x90-byte stride matching a contiguous array) has no
               validated name-string field in its first 0x140 bytes, so
               +0x0c (inherited from the removed VMT-scan code, which
               targeted a different discovery path) is wrong -- it is
               actually a coordinate, and treating it as a name pointer
               would corrupt sector position if ever written back.  Until
               the real name field is found, track sector identity only
               (needed for portal readiness / star clustering); renaming
               is skipped in ce_apply_live_arm below rather than guessed
               again and risk corrupting live galaxy data. */
            if (g_ce_live_sector_count >= 24u) { reason = "sector-table-full"; result = 0u; }
            else if (!ce_region_has_access((const void *)(uintptr_t)sector_ptr, 0x10u, 1)) {
                reason = "sector-unreadable"; result = 0u;
            } else {
                sector_index = g_ce_live_sector_count++;
                g_ce_live_sectors[sector_index].object = sector_ptr;
                g_ce_live_sectors[sector_index].old_name = 0u;
                g_ce_live_sectors[sector_index].second_name = 0u;
                g_ce_live_sectors[sector_index].second_name_index = UINT32_MAX;
                /* Visibility ("explored") flag confirmed live: bit 0x100 of
                   the dword at +0x08 flips from 0 to 1 exactly when RScript
                   calls SectorVisible(sector,1), and nothing else in the
                   object changes (diffed a before/after dump byte-for-byte).
                   The low byte of the same dword is unrelated (already 1
                   before the sector was ever opened) and is preserved, not
                   touched, everywhere this flag is read or written. */
                g_ce_live_sectors[sector_index].old_visible =
                    *(const uint32_t *)(uintptr_t)(sector_ptr + 0x08u) & 0x100u;
                /* Sector "row number" into the vanilla Constellations.Name
                   Lang table: +0x04 tracked sector_index+1 exactly (1..20,
                   zero exceptions) across a live 20-sector survey -- see
                   CEAdapterDumpSectorIndexed. CE_MapSmoke.Lang.txt adds
                   rows 21-44 with this mod's own Second Home sector names
                   under the same Constellations.Name table (Lang.dat
                   sections merge per-key with the base game's, the same
                   way every other CE.* Lang addition in this project
                   already relies on). */
                g_ce_live_sectors[sector_index].old_sector_number =
                    *(const uint32_t *)(uintptr_t)(sector_ptr + 0x04u);
            }
        }
        if (result != 0u) {
            if (g_ce_live_cons[system_index].sector_object == 0u)
                ++g_ce_live_mapped_system_count;
            g_ce_live_cons[system_index].sector_object = sector_ptr;
        }
    }

    if ((uint32_t)InterlockedIncrement(&g_ce_ssc_logged) <= 100u) {
        char diagnostic[160];
        int diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
            "{\"status\":\"set-system-sector\",\"system_index\":%u,"
            "\"sector_ptr\":%u,\"result\":%u,\"reason\":\"%s\"}\r\n",
            system_index, sector_ptr, result, reason);
        if (diagnostic_size > 0) ce_write_text_marker(
            "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    }
    return result;
}

static volatile LONG g_ce_named_dumped[2] = {0, 0};

/* Same bounded, one-shot 0x140-byte read as ce_dump_sector_object, reused
   for planet/station objects so the real name-pointer offset for those
   classes can be confirmed from a live dump instead of assumed. Star
   objects (TCon, from the galaxy's own system list) are already confirmed
   to keep their name pointer at +0x10 (see ce_capture_live_arm/
   ce_apply_live_arm); planets and stations are different classes and may
   or may not share that layout -- this finds out safely, without ever
   scanning unbounded memory. */
static void ce_dump_named_object(const char *kind_label, uint32_t object_ptr) {
    char diagnostic[4096];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"named-object-dump\",\"kind\":\"%s\",\"object_ptr\":%u,\"words\":[",
        kind_label, object_ptr);
    if (written < 0) return;
    used = (size_t)written;
    for (offset = 0u; offset < 0x140u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(object_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(object_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
}

/* kind: 0 = planet (StarPlanets(star,i)), 1 = station (StarRuins(star,i)).
   One-shot per kind, guarded the same way as ce_dump_sector_object -- cheap
   and bounded, safe to call every Turn from the live-arm-switch loop. */
uint32_t CE_CALL CEAdapterDumpNamedObject(uint32_t object_ptr, uint32_t kind) {
    if (object_ptr == 0u || kind > 1u) return 0u;
    if (InterlockedCompareExchange(&g_ce_named_dumped[kind], 1, 0) == 0) {
        ce_dump_named_object(kind == 0u ? "planet" : "station", object_ptr);
    }
    return 1u;
}

static volatile LONG g_ce_ship_snapshot_calls = 0;

/* Same bounded read/string-heuristic dump as ce_dump_named_object, wider
   window (0x400): the player ship (Player(), a TStarShip) is one of the
   larger native object classes touched so far and its item/cargo list is
   not yet known to live within any smaller window already proven safe.
   phase is a caller-supplied tag (0 = before AddItemToShip, 1 = after --
   see the CE_MapSmoke.rson cheat-grant block, which now brackets the
   existing, already-working AddItemToShip call with a snapshot on each
   side) so a single trigger of the cheat produces a matched before/after
   pair in the same marker file, without needing a separately-timed
   external snapshot. Capped at 64 total calls (not gated to one-shot,
   since it needs at least 2 calls per cheat use) purely to keep the log
   file from growing unbounded if the cheat is used repeatedly. */
uint32_t CE_CALL CEAdapterDumpShipSnapshot(uint32_t ship_ptr, uint32_t phase) {
    char diagnostic[16384];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    if (ship_ptr == 0u || (uint32_t)InterlockedIncrement(&g_ce_ship_snapshot_calls) > 64u) return 0u;
    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"ship-snapshot\",\"phase\":%u,\"ship_ptr\":%u,\"words\":[",
        phase, ship_ptr);
    if (written < 0) return 0u;
    used = (size_t)written;
    for (offset = 0u; offset < 0x400u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(ship_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(ship_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
    return 1u;
}

static volatile LONG g_ce_ship_watch_started = 0;
static volatile LONG g_ce_ship_watch_generation = 0;
static uint32_t g_ce_ship_watch_previous[0x100];

static int ce_read_ship_words(uint32_t ship_ptr, uint32_t *words_out) {
    uint32_t offset;
    for (offset = 0u; offset < 0x400u; offset += 4u) {
        if (!ce_region_has_access((const void *)(uintptr_t)(ship_ptr + offset), 4u, 0)) return 0;
        words_out[offset / 4u] = *(const uint32_t *)(uintptr_t)(ship_ptr + offset);
    }
    return 1;
}

/* Passive, generic diff watcher -- unlike CEAdapterDumpShipSnapshot (which
   only fires around this mod's own AddItemToShip call), this catches a
   change caused by ANYTHING, including a vanilla dev cheat (e.g. "ARTS",
   which the user says grants every artifact in the game) that never goes
   through our own Turn-code path at all. Called every Turn from the
   existing "Adapter smoke" block (already runs unconditionally each
   Turn, see CE_MapSmoke.rson); compares the ship's 0x400-byte window
   against what it saw on the previous Turn and, on any difference, dumps
   the full current window plus the exact list of changed byte offsets.
   First call only establishes a baseline (nothing to diff against yet).
   Capped at 64 dumps total, same log-growth reason as
   CEAdapterDumpShipSnapshot. */
uint32_t CE_CALL CEAdapterWatchShipForChanges(uint32_t ship_ptr) {
    uint32_t current[0x100];
    uint32_t changed_offsets[0x100];
    uint32_t changed_count = 0u;
    uint32_t offset;
    char diagnostic[16384];
    int diagnostic_size;
    size_t used = 0;
    int written;
    int first_time;

    if (ship_ptr == 0u || !ce_read_ship_words(ship_ptr, current)) return 0u;
    first_time = InterlockedCompareExchange(&g_ce_ship_watch_started, 1, 0) == 0;
    if (!first_time) {
        for (offset = 0u; offset < 0x100u; ++offset) {
            if (current[offset] != g_ce_ship_watch_previous[offset]) {
                changed_offsets[changed_count++] = offset * 4u;
            }
        }
    }
    memcpy(g_ce_ship_watch_previous, current, sizeof(current));
    if (first_time || changed_count == 0u) return 1u;
    if ((uint32_t)InterlockedIncrement(&g_ce_ship_watch_generation) > 64u) return 1u;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"ship-watch-changed\",\"ship_ptr\":%u,\"changed_count\":%u,\"changed_offsets\":[",
        ship_ptr, changed_count);
    if (written < 0) return 1u;
    used = (size_t)written;
    for (offset = 0u; offset < changed_count && used + 16u < sizeof(diagnostic); ++offset) {
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s%u", offset == 0u ? "" : ",", changed_offsets[offset]);
        if (written > 0) used += (size_t)written;
    }
    written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "],\"words\":[");
    if (written > 0) used += (size_t)written;
    for (offset = 0u; offset < 0x100u && used + 96u < sizeof(diagnostic); ++offset) {
        uint32_t value = current[offset];
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (value >= 0x10000u &&
                ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
            uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
            if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                    ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                preview_len = byte_length / 2u;
                if (preview_len > 20u) preview_len = 20u;
                memcpy(preview, text, preview_len * sizeof(wchar_t));
                has_string = 1;
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset * 4u, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
    return 1u;
}

static volatile LONG g_ce_sector_survey_started = 0;

/* Same bounded-read/string-heuristic shape as ce_dump_named_object, but a
   wider window (0x300 instead of 0x140): the sector's name is confirmed
   absent from the first 0x140 bytes, so this checks further out in case
   an embedded name pointer exists past where the earlier dump stopped.
   Tags each entry with the caller-supplied index (GalaxySectors(i)'s i,
   a plain dword -- no string marshaling needed) so it can be matched
   against the sector's real name, displayed separately via Ether() from
   RScript using ConName(), which needs no new DLL surface at all. */
static void ce_dump_sector_indexed(uint32_t sector_ptr, uint32_t index) {
    char diagnostic[12288];
    int diagnostic_size;
    uint32_t offset;
    size_t used = 0;
    int written;

    written = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"sector-survey\",\"sector_index\":%u,\"object_ptr\":%u,\"words\":[",
        index, sector_ptr);
    if (written < 0) return;
    used = (size_t)written;
    for (offset = 0u; offset < 0x300u && used + 96u < sizeof(diagnostic); offset += 4u) {
        uint32_t value = 0u;
        int has_string = 0;
        wchar_t preview[24];
        uint32_t preview_len = 0u;
        if (ce_region_has_access(
                (const void *)(uintptr_t)(sector_ptr + offset), 4u, 0)) {
            value = *(const uint32_t *)(uintptr_t)(sector_ptr + offset);
            if (value >= 0x10000u &&
                    ce_region_has_access((const void *)(uintptr_t)(value - 4u), 4u, 0)) {
                uint32_t byte_length = *(const uint32_t *)(uintptr_t)(value - 4u);
                if (byte_length >= 2u && byte_length <= 64u && (byte_length & 1u) == 0u &&
                        ce_region_has_access((const void *)(uintptr_t)value, byte_length, 0)) {
                    const wchar_t *text = (const wchar_t *)(uintptr_t)value;
                    preview_len = byte_length / 2u;
                    if (preview_len > 20u) preview_len = 20u;
                    memcpy(preview, text, preview_len * sizeof(wchar_t));
                    has_string = 1;
                }
            }
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
            "%s{\"off\":%u,\"val\":%u", offset == 0u ? "" : ",", offset, value);
        if (written > 0) used += (size_t)written;
        if (has_string && used + 8u + preview_len * 6u < sizeof(diagnostic)) {
            uint32_t k;
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, ",\"s\":\"");
            if (written > 0) used += (size_t)written;
            for (k = 0u; k < preview_len && used + 8u < sizeof(diagnostic); ++k) {
                written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                    "\\u%04x", (unsigned)preview[k]);
                if (written > 0) used += (size_t)written;
            }
            written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "\"");
            if (written > 0) used += (size_t)written;
        }
        written = snprintf(diagnostic + used, sizeof(diagnostic) - used, "}");
        if (written > 0) used += (size_t)written;
    }
    diagnostic_size = snprintf(diagnostic + used, sizeof(diagnostic) - used, "]}\r\n");
    if (diagnostic_size > 0) used += (size_t)diagnostic_size;
    ce_write_text_marker("live-arm-switch.jsonl", diagnostic, used);
}

/* Gate for a one-shot full-galaxy sector survey: returns 1 exactly once
   (ever), so RScript can wrap a "loop every sector, dump + announce its
   name" block in it without needing per-call dedup bookkeeping. Bounded,
   single-object reads per sector (same safe category as every other dump
   this session) -- not a memory scan. */
uint32_t CE_CALL CEAdapterBeginSectorSurvey(void) {
    return (uint32_t)(InterlockedCompareExchange(&g_ce_sector_survey_started, 1, 0) == 0);
}

uint32_t CE_CALL CEAdapterDumpSectorIndexed(uint32_t sector_ptr, uint32_t index) {
    if (sector_ptr == 0u) return 0u;
    ce_dump_sector_indexed(sector_ptr, index);
    return 1u;
}

static volatile LONG g_ce_cluster_survey_started = 0;

/* TGALAXY_LOADFROMSTREAM_FORMAT.md documents a disassembled "Stage 3"
   class (candidate: sectors/TCluster), stored in the galaxy's own list at
   self+0x34 (same list shape as the already-proven star list at
   self+0x2c), whose init function's FIRST action is to read a STRING via
   the documented string-reader (0x0082ff18) straight into its own
   self+0x04 -- i.e. a real embedded name pointer, confirmed by disassembly
   of Rangers.exe, not a live-memory guess. This cannot be the same class
   GalaxySectors()/StarToCon() return: those objects' own +0x04 is a small
   sequential integer (1..20, already tested as a write target with no
   effect on the displayed name), not a valid heap pointer. Whatever
   GalaxySectors() exposes to RScript is most likely a lightweight runtime
   view (TConstellation) distinct from this raw TCluster save-data class.
   Reads galaxy_ptr+0x34 with the exact same ce_read_plain_list() call
   already proven safe for the star list, then bounded-dumps each member
   the same way as every other object this session -- no full-memory scan,
   no new offset guessed without a documented basis. */
uint32_t CE_CALL CEAdapterSurveyClusterList(uint32_t galaxy_ptr) {
    uint32_t cluster_list, cluster_array, cluster_count, i;
    char diagnostic[256];
    int diagnostic_size;
    if (InterlockedCompareExchange(&g_ce_cluster_survey_started, 1, 0) != 0) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x34u), 4u, 0)) {
        const char *failure = "{\"status\":\"cluster-survey-failed\",\"stage\":\"galaxy-field\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", failure, strlen(failure));
        return 0u;
    }
    cluster_list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x34u);
    if (!ce_read_plain_list(cluster_list, &cluster_array, &cluster_count)) {
        const char *failure = "{\"status\":\"cluster-survey-failed\",\"stage\":\"list\"}\r\n";
        ce_write_text_marker("live-arm-switch.jsonl", failure, strlen(failure));
        return 0u;
    }
    diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"cluster-survey-count\",\"count\":%u}\r\n", cluster_count);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    for (i = 0; i < cluster_count && i < 24u; ++i) {
        uint32_t obj;
        if (!ce_region_has_access((const void *)(uintptr_t)(cluster_array + i * 4u), 4u, 0)) break;
        obj = *(const uint32_t *)(uintptr_t)(cluster_array + i * 4u);
        if (obj != 0u) ce_dump_sector_indexed(obj, i);
    }
    return 1u;
}

/* +0x04 tracks sector_index+1 across all 20 captured sectors, with zero
   exceptions -- a live before/after write test is the only way to know
   whether this is really the 1-based row number into Constellations.Name
   (a hypothesis from the decompiled vanilla Lang.dat, which has exactly a
   20-row Constellations.Name table matching this galaxy's 20 sectors) or
   just an unrelated creation-order counter. Bounded single-dword write,
   same risk class as every other confirmed offset this session. */
uint32_t CE_CALL CEAdapterSetSectorIndex(uint32_t sector_ptr, uint32_t new_index) {
    if (sector_ptr == 0u) return 0u;
    if (!ce_region_has_access((const void *)(uintptr_t)(sector_ptr + 4u), 4u, 1)) return 0u;
    *(uint32_t *)(uintptr_t)(sector_ptr + 4u) = new_index;
    return 1u;
}

static volatile LONG g_ce_sector_flag_probed = 0;

/* SectorVisible() can only open a sector via documented RScript, never
   close one -- if the "explored" flag turns out to be a plain byte/dword
   on the sector object (same category as the name-pointer offsets already
   found this way), a direct write could close it too. Find the flag by
   dumping the SAME sector's bytes immediately before and after RScript
   calls SectorVisible(sector,1) on it, then diffing by eye in the log --
   one-shot, fires on the first currently-unexplored sector it finds, and
   deliberately opens exactly that one sector as a side effect (RScript
   only calls this pair around a real SectorVisible(...,1) call). */
uint32_t CE_CALL CEAdapterProbeSectorFlagBefore(uint32_t sector_ptr) {
    if (sector_ptr == 0u) return 0u;
    if (InterlockedCompareExchange(&g_ce_sector_flag_probed, 1, 0) != 0) return 0u;
    ce_dump_named_object("sector-before-open", sector_ptr);
    return 1u;
}

uint32_t CE_CALL CEAdapterProbeSectorFlagAfter(uint32_t sector_ptr) {
    if (sector_ptr == 0u) return 0u;
    ce_dump_named_object("sector-after-open", sector_ptr);
    return 1u;
}

/* Name pointer offset (+0x14) confirmed by CEAdapterDumpNamedObject's live
   dump, not guessed -- see g_ce_second_planet_names above. */
uint32_t CE_CALL CEAdapterCapturePlanetName(uint32_t galaxy_ptr, uint32_t planet_ptr) {
    if (g_ce_live_snapshot_galaxy != galaxy_ptr) return 0u;
    return ce_capture_named_entity(&g_ce_live_planets, &g_ce_live_planet_count,
        &g_ce_live_planet_capacity, planet_ptr, 0x14u,
        g_ce_second_planet_names,
        (uint32_t)(sizeof(g_ce_second_planet_names) / sizeof(g_ce_second_planet_names[0])));
}

/* Name pointer offset (+0x08) confirmed by CEAdapterDumpNamedObject's live
   dump, not guessed -- see g_ce_second_station_names above. */
uint32_t CE_CALL CEAdapterCaptureStationName(uint32_t galaxy_ptr, uint32_t station_ptr) {
    if (g_ce_live_snapshot_galaxy != galaxy_ptr) return 0u;
    return ce_capture_named_entity(&g_ce_live_stations, &g_ce_live_station_count,
        &g_ce_live_station_capacity, station_ptr, 0x08u,
        g_ce_second_station_names,
        (uint32_t)(sizeof(g_ce_second_station_names) / sizeof(g_ce_second_station_names[0])));
}

/* star_owner_value is whatever RScript's own StarOwner(star) call just
   returned (1 == Dominators, per the documented 0=Coalition/1=Dominators/
   2=Pirates scale) -- this DLL has no way to call that built-in itself.
   Idempotent per system_index (first value seen wins) so the captured
   baseline always reflects the true old-arm state, even though the
   per-star Turn-code loop that calls this keeps running every turn
   afterwards, including while Second Home's own suppression is active. */
uint32_t CE_CALL CEAdapterCaptureDominatorFlag(uint32_t system_index, uint32_t star_owner_value) {
    if (g_ce_live_cons == NULL || system_index >= g_ce_live_con_count) return 0u;
    if (g_ce_live_cons[system_index].dominator_captured == 0u) {
        g_ce_live_cons[system_index].old_dominator = star_owner_value == 1u ? 1u : 0u;
        g_ce_live_cons[system_index].dominator_captured = 1u;
    }
    return 1u;
}

uint32_t CE_CALL CEAdapterWasDominator(uint32_t system_index) {
    if (g_ce_live_cons == NULL || system_index >= g_ce_live_con_count) return 0u;
    return g_ce_live_cons[system_index].old_dominator;
}

static volatile LONG g_ce_dominator_count_logged = 0;

/* One-shot diagnostic: dominators are still visible after 0.0.33's fix
   despite the per-turn sweep confirmed running (ship-watch-changed fired
   repeatedly during the Second Home visit). Prime suspect: the dominator
   flag capture -- gated behind ce_capture_live_arm's "first Turn tick
   ever" idempotent guard, same as planet/station name capture -- may run
   too early (before the engine has finished computing real conquest
   state after a save load), permanently freezing every system's captured
   flag at 0 (Coalition) regardless of true ownership. This counts how
   many of the captured systems actually show old_dominator==1, logged
   once so it can be checked without guessing further. */
uint32_t CE_CALL CEAdapterLogDominatorFlagCount(void) {
    uint32_t index, count = 0u;
    char payload[96];
    int size;
    if (InterlockedCompareExchange(&g_ce_dominator_count_logged, 1, 0) != 0) return 0u;
    if (g_ce_live_cons != NULL) {
        for (index = 0; index < g_ce_live_con_count; ++index) {
            if (g_ce_live_cons[index].old_dominator != 0u) ++count;
        }
    }
    size = snprintf(payload, sizeof(payload),
        "{\"status\":\"dominator-flag-count\",\"count\":%u,\"total\":%u}\r\n",
        count, g_ce_live_con_count);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
    return 1u;
}

typedef struct ce_hidden_boss_entry {
    uint32_t ship_ptr;
    uint32_t original_star_ptr;
} ce_hidden_boss_entry;

#define CE_HIDDEN_BOSS_CAPACITY 32u
static ce_hidden_boss_entry g_ce_hidden_bosses[CE_HIDDEN_BOSS_CAPACITY];
static uint32_t g_ce_hidden_boss_count = 0u;

/* Remembers a hidden boss/station ship's original star as a plain raw
   pointer in this DLL's own memory -- the same proven storage pattern
   used throughout this project (g_ce_live_cons etc.), deliberately NOT
   using RScript's ShipAddCustomShipInfo/ShipCustomShipInfoData for this:
   that mechanism's numeric data slots are documented as plain "int"
   values (e.g. CurTurn()+Rnd(...) in every real reference-mod example
   found), never as a native object reference -- storing a raw star
   pointer through it was an untested assumption this project's own
   practice is to avoid shipping without verification. Idempotent per
   ship_ptr (overwrites if already present) so a ship hidden more than
   once (e.g. re-entering Second Home before ever returning) does not
   grow the table or leak an entry. */
uint32_t CE_CALL CEAdapterHideBossShip(uint32_t ship_ptr, uint32_t original_star_ptr) {
    uint32_t index;
    if (ship_ptr == 0u || original_star_ptr == 0u) return 0u;
    for (index = 0; index < g_ce_hidden_boss_count; ++index) {
        if (g_ce_hidden_bosses[index].ship_ptr == ship_ptr) {
            g_ce_hidden_bosses[index].original_star_ptr = original_star_ptr;
            return 1u;
        }
    }
    if (g_ce_hidden_boss_count >= CE_HIDDEN_BOSS_CAPACITY) return 0u;
    g_ce_hidden_bosses[g_ce_hidden_boss_count].ship_ptr = ship_ptr;
    g_ce_hidden_bosses[g_ce_hidden_boss_count].original_star_ptr = original_star_ptr;
    ++g_ce_hidden_boss_count;
    return 1u;
}

/* Returns the remembered original star for ship_ptr (0 if none), and
   removes the entry (swap-with-last) so it is only ever consumed once. */
uint32_t CE_CALL CEAdapterRestoreBossShip(uint32_t ship_ptr) {
    uint32_t index;
    for (index = 0; index < g_ce_hidden_boss_count; ++index) {
        if (g_ce_hidden_bosses[index].ship_ptr == ship_ptr) {
            uint32_t original_star = g_ce_hidden_bosses[index].original_star_ptr;
            g_ce_hidden_bosses[index] = g_ce_hidden_bosses[g_ce_hidden_boss_count - 1u];
            --g_ce_hidden_boss_count;
            return original_star;
        }
    }
    return 0u;
}

/* Same idea as CEAdapterRestoreBossShip but for the restore-side loop,
   which does not already know each ship_ptr up front -- iterates whatever
   is currently in the table by position instead. Returns 0 once index is
   past the end (safe loop terminator for RScript's while-loop style). */
uint32_t CE_CALL CEAdapterHiddenBossCount(void) {
    return g_ce_hidden_boss_count;
}

uint32_t CE_CALL CEAdapterHiddenBossShipAt(uint32_t index) {
    if (index >= g_ce_hidden_boss_count) return 0u;
    return g_ce_hidden_bosses[index].ship_ptr;
}

/* Fine-grained progress marker for live bisection without needing another
   rebuild-test round-trip: writes {"status":"checkpoint","id":N} to the
   debug log. Placed at each major step of the arm-switch-completion
   Dominator/boss handling in CE_MapSmoke.rson so that if the game crashes
   again, the log's last checkpoint id pinpoints exactly which step was
   reached, the same way the project's own proven "strip Turn-code down,
   add pieces back" bisection technique works, but in one pass instead of
   several rebuild cycles. */
uint32_t CE_CALL CEAdapterLogCheckpoint(uint32_t id) {
    char payload[64];
    int size = snprintf(payload, sizeof(payload), "{\"status\":\"checkpoint\",\"id\":%u}\r\n", id);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
    return 1u;
}

uint32_t CE_CALL CEAdapterLogCheckpointValue(uint32_t id, uint32_t value) {
    char payload[80];
    int size = snprintf(payload, sizeof(payload),
        "{\"status\":\"checkpoint\",\"id\":%u,\"value\":%u}\r\n", id, value);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
    return 1u;
}

/* Two independent argument changes to the BuildListOfNewShips call site
   (sinceId 0->1, then typeSet/raceSet swapped to the working t_Kling
   filter) both still crashed at the exact same checkpoint, with the crash
   never reaching just past the call -- pointing at WHEN it is called
   (nested in NextDay's own call stack, on the exact Turn the portal
   transition just completed) rather than WHAT arguments it is given.
   Deferring the sweep to the very next unconditional per-Turn tick (the
   "Adapter smoke" block, which fires far more often than once per
   calendar day during normal flight, so the delay is imperceptible)
   moves the call off that uniquely fragile moment -- the same fix shape
   already proven for ce_spawn_sector_name_helper's CreateProcess call. */
static volatile LONG g_ce_pending_ship_sweep_arm = -1;

uint32_t CE_CALL CEAdapterSetPendingShipSweep(uint32_t second_home) {
    InterlockedExchange(&g_ce_pending_ship_sweep_arm, second_home != 0u ? 1 : 0);
    return 1u;
}

/* Returns 2 if nothing is pending (RScript-friendly sentinel distinct
   from the real 0/1 arm values), consuming the flag on any non-empty
   read so it only ever fires once per arm switch. */
uint32_t CE_CALL CEAdapterConsumePendingShipSweep(void) {
    LONG value = InterlockedExchange(&g_ce_pending_ship_sweep_arm, -1);
    return value < 0 ? 2u : (uint32_t)value;
}

/* The artifact is enabled only for a party that actually passed through the
   dual-newgame transaction.  Old saves have no independent destination
   sidecar; silently falling back to the archived pointer swap would show a
   renamed clone and can corrupt global references, so it is deliberately
   rejected instead. */
uint32_t CE_CALL CEAdapterPortalReady(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t ready = ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref) &&
        InterlockedCompareExchange(&g_ce_dual_newgame_status, 0, 0) == 2 &&
        g_ce_first_arm_save_path != 0u && g_ce_second_home_save_path != 0u &&
        InterlockedCompareExchange(&g_ce_portal_status, 0, 0) == 0 ? 1u : 0u;
    char diagnostic[192];
    int diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"portal-ready-check\",\"ready\":%s,\"portal_status\":%ld,"
        "\"dual_newgame_status\":%ld}\r\n",
        ready ? "true" : "false",
        (long)InterlockedCompareExchange(&g_ce_portal_status, 0, 0),
        (long)InterlockedCompareExchange(&g_ce_dual_newgame_status, 0, 0));
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    return ready;
}

uint32_t CE_CALL CEAdapterPortalStatus(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_portal_status, 0, 0);
}

/* KEY/KEYMOD come straight from RScript's OnKey (Shift=1, Ctrl=2, Alt=4);
   no reference mod calls ShipAddCustomShipInfo or any other engine-mutating
   API from interface code (OnKey/OnPressCode) -- every real example is
   from act-code or a dialog handler, which fire far less often and never
   from three duplicate panels at once. This function itself does nothing
   but advance a plain counter (safe to call from OnKey on every keystroke,
   even 2-3 times per key from duplicate panels); the actual item grant
   still runs from the Turn-code poll in CE_MapSmoke.rson, the same
   proven-safe context every other heavy call in this mod uses -- that part
   is unchanged.

   What changed: this used to always return 0, leaving the player waiting
   for a real day to pass before Turn-code ever re-ran and noticed the
   trigger. Confirmed via the decompiled RScript function reference
   (references\Universe-Original-Mods-Source\Script functions list.txt,
   "Ход (даты)" section, right next to CurTurn -- a function this mod
   already calls elsewhere) that ForceNextDay() is a real, documented,
   no-argument RScript function that forces the next Turn to run
   immediately. Now returns 1 exactly on the keystroke that completes the
   sequence, so CE_MapSmoke.Main.txt can call ForceNextDay() right there --
   still never touching ship/cargo state itself from OnKey, just nudging
   the clock forward so the ALREADY-safe Turn-code path (which does the
   real mutation) fires now instead of whenever the player next skips a
   day. This still needs a real live-game check before it can be trusted:
   TGalaxy.NextDay is the exact code path that has crashed this project
   multiple times before when entered in a nested/reentrant way (see the
   DllMain/ce_hook_thread_proc comments), and OnKey firing from three
   duplicate panels at once means ForceNextDay could plausibly be invoked
   more than once for a single keystroke. */
uint32_t CE_CALL CEAdapterAdvanceCheatCode(uint32_t key, uint32_t keymod) {
    static const uint32_t cheat[] = {'E', 'L', 'T', 'A', 'N'};
    char diagnostic[128];
    int diagnostic_size = snprintf(diagnostic, sizeof(diagnostic),
        "{\"status\":\"cheat-key\",\"key\":%u,\"keymod\":%u,\"progress_before\":%u}\r\n",
        key, keymod, g_ce_cheat_progress);
    if (diagnostic_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", diagnostic, (size_t)diagnostic_size);
    if (keymod != 3u || key < 'A' || key > 'Z') return 0u;
    if (key == cheat[g_ce_cheat_progress]) {
        ++g_ce_cheat_progress;
    } else {
        g_ce_cheat_progress = key == cheat[0] ? 1u : 0u;
    }
    if (g_ce_cheat_progress == sizeof(cheat) / sizeof(cheat[0])) {
        g_ce_cheat_progress = 0u;
        InterlockedExchange(&g_ce_cheat_triggered, 1);
        ce_write_text_marker("live-arm-switch.jsonl",
            "{\"status\":\"cheat-triggered\"}\r\n", 30u);
        return 1u;
    }
    return 0u;
}

uint32_t CE_CALL CEAdapterConsumeCheatTrigger(void) {
    return (uint32_t)InterlockedExchange(&g_ce_cheat_triggered, 0);
}

/* Which arm is on screen, asked of the world instead of a flag.

   g_ce_active_arm is a DLL global, so it does not survive the player
   quitting and starting the game again. When it disagreed with reality the
   portal wrote the wrong world into the wrong sidecar, and an ordinary
   galaxy landed on top of the authored Eltan map -- vanilla system names,
   vanilla planets, and every Second Home check in the script silently
   false. Star zero of that map is named by tools/make_second_home.py and by
   nothing else, so it answers the question without a flag to go stale.

   Systems live in galaxy+0x2c (confirmed above, against GalaxyEye) and TStar
   keeps its name pointer at +0x10, with the Delphi WideString byte length in
   the word before the characters. */
static int ce_galaxy_is_second_home(uint32_t galaxy_ptr) {
    static const wchar_t marker[] = L"Эльтанская Рана";
    const uint32_t marker_bytes =
        (uint32_t)((sizeof(marker) / sizeof(marker[0]) - 1u) * sizeof(wchar_t));
    uint32_t list, array, count, star, name, length;
    if (galaxy_ptr == 0u ||
            !ce_region_has_access(
                (const void *)(uintptr_t)(galaxy_ptr + 0x2cu), 4u, 0)) return 0;
    list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    if (!ce_read_plain_list(list, &array, &count)) return 0;
    star = *(const uint32_t *)(uintptr_t)array;
    if (!ce_region_has_access((const void *)(uintptr_t)(star + 0x10u), 4u, 0)) return 0;
    name = *(const uint32_t *)(uintptr_t)(star + 0x10u);
    if (name < 4u ||
            !ce_region_has_access((const void *)(uintptr_t)(name - 4u), 4u, 0)) return 0;
    length = *(const uint32_t *)(uintptr_t)(name - 4u);
    if (length != marker_bytes ||
            !ce_region_has_access((const void *)(uintptr_t)name, length, 0)) return 0;
    return memcmp((const void *)(uintptr_t)name, marker, length) == 0;
}

uint32_t CE_CALL CEAdapterGalaxyIsSecondHome(uint32_t galaxy_ptr) {
    return ce_galaxy_is_second_home(galaxy_ptr) ? 1u : 0u;
}
uint32_t CE_CALL CEAdapterRegisterPortal(uint32_t galaxy_ptr, uint32_t hole_id) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t active_arm;
    uint32_t source_path;
    int saved;
    char report[192];
    int report_size;
    if (hole_id == 0u || !CEAdapterPortalReady(galaxy_ptr) ||
            InterlockedCompareExchange(&g_ce_portal_status, 1, 0) != 0) return 0;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref)) {
        InterlockedExchange(&g_ce_portal_status, 0);
        return 0;
    }
    /* Ask the galaxy, not the flag. Getting this backwards overwrites one of
       the two sidecars with the other arm's world, and one of them is
       authored content that cannot be regenerated from inside the game. */
    active_arm = ce_galaxy_is_second_home(galaxy_ptr) ? 1u : 0u;
    InterlockedExchange(&g_ce_active_arm, (LONG)active_arm);
    source_path = active_arm == 0u
        ? g_ce_first_arm_save_path : g_ce_second_home_save_path;
    /* Registration runs from the artifact's OnUseCode, before the player
       flies into the hole and outside NextDay's fragile Turn stack.  Save
       the complete active world here so returning later preserves its
       economy and the traveler's latest source-arm state. */
    saved = ce_save_complete_game(
        module_base, source_path,
        active_arm == 0u ? L"Дети Эльтан: Первый рукав" : L"Дети Эльтан: Второй Дом");
    if (!saved) {
        InterlockedExchange(&g_ce_portal_status, 0);
        ce_write_text_marker("dual-newgame.jsonl",
            "{\"status\":\"portal-source-save-failed\"}\r\n", 40u);
        return 0;
    }
    InterlockedExchange(&g_ce_portal_galaxy_ptr, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_portal_hole_id, (LONG)hole_id);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"portal-opened\",\"hole_id\":%u,\"arm\":\"%s\"}\r\n",
        hole_id, InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0
            ? "OLD_ARM" : "SECOND_HOME");
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return 1;
}

uint32_t CE_CALL CEAdapterEnterRegisteredPortal(uint32_t galaxy_ptr, uint32_t hole_id) {
    char report[192];
    int report_size;
    uint32_t registered_hole_id =
        (uint32_t)InterlockedCompareExchange(&g_ce_portal_hole_id, 0, 0);
    /* ShipOrderObj() is not stable at the instant the player crosses a hole:
       the engine may clear the movement order or return the player object.
       A zero ID therefore means "the portal registered by this adapter".
       The galaxy pointer and armed-state checks below still prevent unrelated
       black holes from triggering an inter-arm transition. */
    if (hole_id == 0u) hole_id = registered_hole_id;
    if (hole_id == 0u ||
            registered_hole_id != hole_id ||
            (uint32_t)InterlockedCompareExchange(&g_ce_portal_galaxy_ptr, 0, 0) != galaxy_ptr ||
            InterlockedCompareExchange(&g_ce_portal_status, 2, 1) != 1) return 0;
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"portal-entered\",\"hole_id\":%u,\"phase\":\"awaiting-exit\"}\r\n",
        hole_id);
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return 1;
}

/* Called only after ShipInHole() has flipped back to false, so the native
   entry animation has finished.  This function does NOT load or delete
   anything on the Turn stack.  It selects the destination sidecar in the
   exact global field TThreadGameLoad.Execute reads; RScript then exits to
   FormChange('GameLoad'), giving the transition the game's own full-screen
   progress animation and worker lifecycle. */
uint32_t CE_CALL CEAdapterCompleteRegisteredPortal(uint32_t galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t class_ref;
    uint32_t target_path;
    char report[192];
    int report_size;
    int entering;
    uint32_t ok;
    (void)turn;
    if ((uint32_t)InterlockedCompareExchange(&g_ce_portal_galaxy_ptr, 0, 0) != galaxy_ptr ||
            InterlockedCompareExchange(&g_ce_portal_status, 3, 2) != 2) return 0;
    if (InterlockedCompareExchange(&g_ce_live_switch_lock, 1, 0) != 0) {
        InterlockedExchange(&g_ce_portal_status, 2);
        return 0;
    }
    entering = InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0;
    /* ANSI variant on purpose: the game-load path cell is an AnsiString the
       engine widens itself -- see ce_make_immortal_ansi's comment. */
    target_path = entering
        ? g_ce_second_home_save_path_ansi : g_ce_first_arm_save_path_ansi;
    ok = ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &class_ref) &&
        ce_set_native_game_load_path(module_base, target_path);
    if (ok) {
        /* The validation LoadGame performed during new-party creation
           succeeds with OLD_ARM active.  The portal path previously exposed
           SECOND_HOME before TThreadGameLoad had even started, allowing
           load-time imports to observe and mutate a half-built target.
           CEAdapterConsumePendingArrival commits the remembered direction on
           the first Turn that runs after a successful load. */
        InterlockedExchange(&g_ce_active_arm, 0);
        /* Tell the GameLoad form's gate stub that the load it is about to
           run belongs to this mod, so it nominates StarMap afterwards
           instead of leaving the engine with no follow-up form. */
        InterlockedExchange(&g_ce_portal_load_pending, 1);
        CEAdapterRequestSectorLabelSpawn(entering ? 1u : 0u);
    }
    InterlockedExchange(&g_ce_portal_hole_id, 0);
    InterlockedExchange(&g_ce_portal_galaxy_ptr, 0);
    InterlockedExchange(&g_ce_portal_status, ok ? 0 : 2);
    InterlockedExchange(&g_ce_live_switch_lock, 0);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"%s\",\"arm\":\"%s\"}\r\n",
        ok ? "portal-load-prepared" : "portal-load-prepare-failed",
        InterlockedCompareExchange(&g_ce_active_arm, 0, 0) == 0
            ? "OLD_ARM" : "SECOND_HOME");
    if (report_size > 0) ce_write_text_marker(
        "live-arm-switch.jsonl", report, (size_t)report_size);
    return ok;
}

__attribute__((used)) static void CE_CALL ce_transform_loaded_second_home(
    uint32_t galaxy_ptr, uint32_t reader
) {
    uint32_t list;
    uint32_t array;
    uint32_t count;
    uint32_t index;
    uint32_t transformed = 0;
    uint32_t seed;
    float min_x = 10000000.0f;
    float max_x = -10000000.0f;
    float min_y = 10000000.0f;
    float max_y = -10000000.0f;
    char report[256];
    int report_size;

    (void)reader;
    if (InterlockedExchange(&g_ce_load_transform_armed, 0) == 0) return;
    seed = (uint32_t)InterlockedCompareExchange(&g_ce_load_transform_seed, 0, 0);
    if (!ce_region_has_access((const void *)(uintptr_t)(galaxy_ptr + 0x2cu), 4u, 0)) {
        ce_write_text_marker("second-home-transform.jsonl",
            "{\"status\":\"galaxy-unreadable\"}\r\n", 32u);
        return;
    }
    list = *(const uint32_t *)(uintptr_t)(galaxy_ptr + 0x2cu);
    if (!ce_region_has_access((const void *)(uintptr_t)list, 12u, 0)) return;
    array = *(const uint32_t *)(uintptr_t)(list + 4u);
    count = *(const uint32_t *)(uintptr_t)(list + 8u);
    if (count < 2u || count > 10000u ||
        !ce_region_has_access((const void *)(uintptr_t)array, count * 4u, 0)) return;

    for (index = 0; index < count; ++index) {
        uint32_t con = *(const uint32_t *)(uintptr_t)(array + index * 4u);
        float x;
        float y;
        if (!ce_region_has_access((const void *)(uintptr_t)con, 0x1cu, 0)) continue;
        memcpy(&x, (const void *)(uintptr_t)(con + 0x14u), sizeof(x));
        memcpy(&y, (const void *)(uintptr_t)(con + 0x18u), sizeof(y));
        if (x < -10000000.0f || x > 10000000.0f ||
            y < -10000000.0f || y > 10000000.0f) continue;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    if (min_x >= max_x || min_y >= max_y) return;

    for (index = 0; index < count; ++index) {
        uint32_t con = *(const uint32_t *)(uintptr_t)(array + index * 4u);
        uint32_t id;
        uint32_t hx;
        uint32_t hy;
        float x;
        float y;
        if (!ce_region_has_access((const void *)(uintptr_t)con, 0x1cu, 1)) continue;
        id = *(const uint32_t *)(uintptr_t)(con + 0x04u);
        hx = ce_mix32(seed ^ id ^ (index * 0x9e3779b9u));
        hy = ce_mix32((seed + 0x85ebca6bu) ^ id ^ (index * 0xc2b2ae35u));
        x = min_x + ((float)(hx & 0xffffu) / 65535.0f) * (max_x - min_x);
        y = min_y + ((float)(hy & 0xffffu) / 65535.0f) * (max_y - min_y);
        memcpy((void *)(uintptr_t)(con + 0x14u), &x, sizeof(x));
        memcpy((void *)(uintptr_t)(con + 0x18u), &y, sizeof(y));
        ++transformed;
    }
    InterlockedExchange(&g_ce_active_arm, 1);
    report_size = snprintf(report, sizeof(report),
        "{\"status\":\"transformed\",\"abi\":%u,\"seed\":%u,"
        "\"count\":%u,\"bounds\":[%.2f,%.2f,%.2f,%.2f]}\r\n",
        CEAdapterAbiVersion(), seed, transformed, min_x, max_x, min_y, max_y);
    if (report_size > 0) ce_write_text_marker(
        "second-home-transform.jsonl", report, (size_t)report_size
    );
}

#if defined(__i386__)
__attribute__((naked)) static void ce_tgalaxy_load_hook(void) {
    __asm__ volatile(
        "pushl %edx\n\t"
        "pushl %eax\n\t"
        "call *_g_ce_load_trampoline\n\t"
        "popl %ecx\n\t"
        "popl %edx\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"
        "call _ce_transform_loaded_second_home\n\t"
        "addl $8, %esp\n\t"
        "ret\n\t"
    );
}
#endif

uint32_t CE_CALL CEAdapterPollSecondHomeTransform(
    uint32_t galaxy_ptr, uint32_t seed
) {
#if defined(__i386__)
    static const unsigned char load_signature[] = {
        0x55, 0x8b, 0xec, 0xb9, 0x44, 0x00, 0x00, 0x00,
        0x6a, 0x00, 0x6a, 0x00, 0x49, 0x75, 0xf9, 0x51
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[8];
    unsigned char *trampoline;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;
    int32_t relative;

    /* Existing saves retain their compiled Turn graph.  ABI 11 saves call
       this export already, so use it as the compatibility bridge that
       installs the ABI 12 portal hotkey without requiring a new campaign. */
    CEAdapterInstallLiveArmSwitch(galaxy_ptr, seed);

    if (GetFileAttributesA("C:\\ce_debug\\arm-second-home.flag") == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    target = (unsigned char *)(module_base + CE_RVA_TGALAXY_LOAD_FROM_STREAM);
    hook_state = InterlockedCompareExchange(&g_ce_load_hook_installed, -1, 0);
    if (hook_state == 0) {
        if (memcmp(target, load_signature, sizeof(load_signature)) != 0) {
            InterlockedExchange(&g_ce_load_hook_installed, 0);
            return 0;
        }
        trampoline = (unsigned char *)VirtualAlloc(
            NULL, 13u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        if (trampoline == NULL) {
            InterlockedExchange(&g_ce_load_hook_installed, 0);
            return 0;
        }
        memcpy(trampoline, target, 8u);
        trampoline[8] = 0xe9;
        relative = (int32_t)((target + 8u) - (trampoline + 13u));
        memcpy(trampoline + 9u, &relative, sizeof(relative));
        g_ce_load_trampoline = trampoline;
        FlushInstructionCache(GetCurrentProcess(), trampoline, 13u);
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xe9;
        relative = (int32_t)((unsigned char *)(uintptr_t)ce_tgalaxy_load_hook -
            (target + 5u));
        memcpy(patch + 1u, &relative, sizeof(relative));
        if (!VirtualProtect(target, 8u, PAGE_EXECUTE_READWRITE, &old_protect)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            g_ce_load_trampoline = NULL;
            InterlockedExchange(&g_ce_load_hook_installed, 0);
            return 0;
        }
        memcpy(target, patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
        VirtualProtect(target, 8u, old_protect, &ignored_protect);
        InterlockedExchange(&g_ce_load_hook_installed, 1);
    } else if (hook_state != 1) {
        return 0;
    }
    InterlockedExchange(&g_ce_load_transform_seed, (LONG)seed);
    InterlockedExchange(&g_ce_load_transform_armed, 1);
    DeleteFileA("C:\\ce_debug\\arm-second-home.flag");
    ce_write_text_marker("second-home-transform.jsonl",
        "{\"status\":\"armed\"}\r\n", 20u);
    return 1;
#else
    (void)galaxy_ptr;
    (void)seed;
    return 0;
#endif
}

/* The captured buffer's first bytes turned out to be unrelated rich-text
   (a system encyclopedia entry, "sys_storage1" + color/table markup), not
   TGalaxy's own "mod set" string LoadFromStream expects first -- meaning
   the TMemoryStream this hook observes is shared by the engine's whole
   save-game sequence, and TGalaxy.SaveToStream is only one section written
   partway through it, not from position 0. Recording the stream's own
   read/write position at the MOMENT SaveToStream is entered (before it
   writes anything) gives the correct starting offset to slice out just
   TGalaxy's own section, so replaying it through LoadFromStream at
   (sliced) position 0 lines up correctly instead of desyncing immediately. */
static volatile LONG g_ce_save_start_position = -1;

__attribute__((used)) static void CE_CALL ce_capture_save_start_position(uint32_t stream_ptr) {
    if (ce_region_has_access((const void *)(uintptr_t)(stream_ptr + 0x0cu), 4u, 0)) {
        InterlockedExchange(&g_ce_save_start_position,
            (LONG)(*(const uint32_t *)(uintptr_t)(stream_ptr + 0x0cu)));
    } else {
        InterlockedExchange(&g_ce_save_start_position, -1);
    }
}

/* SaveToStream cannot safely be called from a Turn callback: the real game
   test deadlocked before producing a byte.  The safe place is the engine's
   own save operation.  This detour lets the original serializer run first,
   then copies its completed buffer without re-entering any Delphi code.

   Six bytes are displaced because the first three x86 instructions are
   1+2+3 bytes.  The trampoline executes those complete instructions and
   jumps back to SaveToStream+6. */
__attribute__((used)) static void CE_CALL ce_capture_completed_galaxy_save(
    uint32_t galaxy_ptr, uint32_t buffer
) {
    uint32_t expected;
    uint32_t length;
    uint32_t capacity;
    uint32_t position;
    uint32_t data;
    uint32_t hash;
    char report[224];
    int report_size;
    int wrote;

    if (InterlockedCompareExchange(&g_ce_save_hook_armed, 0, 0) == 0) return;
    expected = (uint32_t)InterlockedCompareExchange(
        &g_ce_save_hook_expected_galaxy, 0, 0
    );
    if (expected == 0 || galaxy_ptr != expected) return;
    if (InterlockedExchange(&g_ce_save_hook_armed, 0) == 0) return;
    if (!ce_region_has_access((const void *)(uintptr_t)buffer, 0x14u, 0)) {
        ce_write_text_marker("galaxy-save-snapshot.jsonl",
            "{\"status\":\"invalid-buffer\"}\r\n", 29u);
        return;
    }

    length = *(const uint32_t *)(uintptr_t)(buffer + 0x04u);
    capacity = *(const uint32_t *)(uintptr_t)(buffer + 0x08u);
    position = *(const uint32_t *)(uintptr_t)(buffer + 0x0cu);
    data = *(const uint32_t *)(uintptr_t)(buffer + 0x10u);
    if (length == 0 || length > capacity || position != length ||
        length > 256u * 1024u * 1024u ||
        !ce_region_has_access((const void *)(uintptr_t)data, length, 0)) {
        report_size = snprintf(report, sizeof(report),
            "{\"status\":\"invalid-layout\",\"length\":%u,"
            "\"position\":%u,\"capacity\":%u}\r\n",
            length, position, capacity);
        if (report_size > 0) ce_write_text_marker(
            "galaxy-save-snapshot.jsonl", report, (size_t)report_size
        );
        return;
    }

    {
        LONG start_position = InterlockedCompareExchange(&g_ce_save_start_position, 0, 0);
        uint32_t slice_offset = (start_position >= 0 && (uint32_t)start_position <= length)
            ? (uint32_t)start_position : 0u;
        uint32_t slice_length = length - slice_offset;
        const unsigned char *slice_data = (const unsigned char *)(uintptr_t)data + slice_offset;

        hash = ce_fnv1a32(slice_data, slice_length);
        wrote = ce_write_binary_file("C:\\ce_debug", "galaxy-save-snapshot.bin",
            slice_data, slice_length);
        report_size = snprintf(report, sizeof(report),
            "{\"status\":\"%s\",\"abi\":%u,\"galaxy_ptr\":%u,"
            "\"length\":%u,\"slice_offset\":%u,\"slice_length\":%u,\"fnv1a32\":%u}\r\n",
            wrote ? "captured" : "write-failed", CEAdapterAbiVersion(),
            galaxy_ptr, length, slice_offset, slice_length, hash);
        if (report_size > 0) ce_write_text_marker(
            "galaxy-save-snapshot.jsonl", report, (size_t)report_size
        );
        if (wrote) InterlockedExchange(&g_ce_snapshot_done, 1);
    }
}

#if defined(__i386__)
__attribute__((naked)) static void ce_tgalaxy_save_hook(void) {
    __asm__ volatile(
        "pushl %edx\n\t"
        "pushl %eax\n\t"
        "pushl %edx\n\t"
        "call _ce_capture_save_start_position\n\t"
        "addl $4, %esp\n\t"
        "movl (%esp), %eax\n\t"
        "movl 4(%esp), %edx\n\t"
        "call *_g_ce_save_trampoline\n\t"
        "popl %ecx\n\t"
        "popl %edx\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"
        "call _ce_capture_completed_galaxy_save\n\t"
        "addl $8, %esp\n\t"
        "ret\n\t"
    );
}
#endif

uint32_t CE_CALL CEAdapterArmGalaxySaveSnapshot(uint32_t galaxy_ptr) {
#if defined(__i386__)
    static const unsigned char save_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xa0, 0x33, 0xc9,
        0x89, 0x4d, 0xa0, 0x89, 0x55, 0xf8, 0x89, 0x45
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[6];
    unsigned char *trampoline;
    DWORD old_protect;
    DWORD ignored_protect;
    LONG hook_state;
    int32_t relative;

    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) return 0;
    target = (unsigned char *)(module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM);
    hook_state = InterlockedCompareExchange(&g_ce_save_hook_installed, -1, 0);
    if (hook_state == 0) {
        if (memcmp(target, save_signature, sizeof(save_signature)) != 0) {
            InterlockedExchange(&g_ce_save_hook_installed, 0);
            return 0;
        }
        trampoline = (unsigned char *)VirtualAlloc(
            NULL, 11u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        if (trampoline == NULL) {
            InterlockedExchange(&g_ce_save_hook_installed, 0);
            return 0;
        }
        memcpy(trampoline, target, 6u);
        trampoline[6] = 0xe9;
        relative = (int32_t)((target + 6u) - (trampoline + 11u));
        memcpy(trampoline + 7u, &relative, sizeof(relative));
        g_ce_save_trampoline = trampoline;
        FlushInstructionCache(GetCurrentProcess(), trampoline, 11u);

        patch[0] = 0xe9;
        relative = (int32_t)((unsigned char *)(uintptr_t)ce_tgalaxy_save_hook -
            (target + 5u));
        memcpy(patch + 1u, &relative, sizeof(relative));
        patch[5] = 0x90;
        if (!VirtualProtect(target, 6u, PAGE_EXECUTE_READWRITE, &old_protect)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            g_ce_save_trampoline = NULL;
            InterlockedExchange(&g_ce_save_hook_installed, 0);
            return 0;
        }
        memcpy(target, patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
        VirtualProtect(target, 6u, old_protect, &ignored_protect);
        InterlockedExchange(&g_ce_save_hook_installed, 1);
    } else if (hook_state != 1) {
        return 0;
    }

    InterlockedExchange(&g_ce_save_hook_expected_galaxy, (LONG)galaxy_ptr);
    InterlockedExchange(&g_ce_save_hook_armed, 1);
    ce_write_text_marker("galaxy-save-snapshot.jsonl",
        "{\"status\":\"armed\"}\r\n", 20u);
    return 1;
#else
    (void)galaxy_ptr;
    return 0;
#endif
}

/* Phase B0: the bare second galaxy built by CEAdapterCreateAndEnterSecondGalaxy
   only has stars (GenerateStars alone, no sectors/economy/names -- the
   ~8KB "orchestrator" that fills those in during the engine's own bootstrap
   has never been reverse engineered). Rather than reproduce that,
   feed a real, fully-populated galaxy's own serialized bytes (captured by
   the SaveToStream detour above, C:\ce_debug\galaxy-save-snapshot.bin)
   into TGalaxy.LoadFromStream (VA 0x0083b6cc, documented in
   docs/TGALAXY_LOADFROMSTREAM_FORMAT.md) called directly against the
   second galaxy instance. The reader argument that method reads is not a
   real Delphi object -- no vtable dispatch, just flat field reads at
   +0x04 (size), +0x0c (cursor), +0x10 (data pointer) -- so it can be
   fabricated as a plain heap block, the same trick already used for
   ce_make_immortal_unicode's strings.

   This is the first time this adapter has ever called into
   LoadFromStream's ~8.8KB of largely undocumented Delphi bytecode, so it
   is wrapped in the same VEH+setjmp recovery net as every other risky
   engine call in this file (see the TCon constructor fault above) and the
   CE_RVA_CON_CONTEXT_CELL guard is zeroed for the duration, exactly like
   the single-TCon-construction case, since LoadFromStream rebuilds many
   TCon-like sub-objects in one call and would touch that same shared
   next-id counter repeatedly. */
uint32_t CE_CALL CEAdapterLoadSnapshotIntoSecondGalaxy(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t second_galaxy;
    unsigned char *file_data = NULL;
    uint32_t file_length = 0;
    unsigned char *reader_block;
    uint32_t result = 0;

    if (InterlockedCompareExchange(&g_ce_second_snapshot_loaded, 0, 0) != 0) {
        return 2;
    }
    second_galaxy = (uint32_t)InterlockedCompareExchange(&g_ce_second_galaxy_ptr, 0, 0);
    if (second_galaxy == 0 ||
        !ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        ce_write_progress("load-snapshot:abort:resolve-failed");
        return 0;
    }
    if (!ce_read_binary_file("C:\\ce_debug\\galaxy-save-snapshot.bin", &file_data, &file_length) ||
        file_length == 0) {
        ce_write_progress("load-snapshot:abort:no-file");
        return 0;
    }

    reader_block = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x18u);
    if (reader_block == NULL) {
        HeapFree(GetProcessHeap(), 0, file_data);
        ce_write_progress("load-snapshot:abort:reader-alloc-failed");
        return 0;
    }
    *(uint32_t *)(void *)(reader_block + 0x04u) = file_length;
    *(uint32_t *)(void *)(reader_block + 0x08u) = file_length;
    *(uint32_t *)(void *)(reader_block + 0x0cu) = 0u;
    *(uint32_t *)(void *)(reader_block + 0x10u) = (uint32_t)(uintptr_t)file_data;

    {
        volatile uint32_t context_saved = 0;
        volatile uint32_t original_context = 0;
        uintptr_t context_cell = module_base + CE_RVA_CON_CONTEXT_CELL;

        if (ce_region_has_access((const void *)context_cell, 4u, 0)) {
            original_context = *(const uint32_t *)context_cell;
            context_saved = 1;
        }

        ce_ensure_veh_installed();
        if (setjmp(g_ce_recovery_point) != 0) {
            if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
                *(uint32_t *)context_cell = original_context;
            }
            ce_write_fault_report("load-snapshot:FAULTED");
            result = 0;
        } else {
            InterlockedExchange(&g_ce_guard_active, 1);
            if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
                *(uint32_t *)context_cell = 0;
            }
            ce_write_progress("load-snapshot:before-call");
            ce_call_delphi_method_dword(
                second_galaxy, (uint32_t)(uintptr_t)reader_block,
                module_base + CE_RVA_TGALAXY_LOAD_FROM_STREAM);
            ce_write_progress("load-snapshot:after-call");
            if (context_saved && ce_region_has_access((const void *)context_cell, 4u, 1)) {
                *(uint32_t *)context_cell = original_context;
            }
            InterlockedExchange(&g_ce_guard_active, 0);
            InterlockedExchange(&g_ce_second_snapshot_loaded, 1);
            result = 1;
        }
    }

    HeapFree(GetProcessHeap(), 0, reader_block);
    HeapFree(GetProcessHeap(), 0, file_data);
    return result;
}

/* One-shot system rename for a genuinely separate second TGalaxy -- unlike
   ce_apply_live_arm (built for the archived cosmetic scheme, where both
   arms were the SAME shared galaxy and every rename needed an old_name to
   revert to on toggle), the clone loaded by
   CEAdapterLoadSnapshotIntoSecondGalaxy lives in its own memory entirely;
   the original galaxy's objects are never touched, so there is nothing to
   revert and no toggle state to track. Just overwrite every system's name
   pointer once, right after a successful load. [self+0x2c] is the TCon
   list RScript's GalaxyStar()/StarToCon() actually walk (name at +0x10,
   confirmed by ce_capture_live_arm's own already-working use of the same
   offset) -- distinct from [self+0x164]'s TStar list that
   LoadFromStream/CEAdapterGetGeneratedStarCount deal with internally.
   Sector-level (large region) labels are intentionally out of scope here:
   those live in a separate BlockPar config-tree row object that the old
   scheme only ever reached via an out-of-process scan (ce_spawn_sector_
   name_helper) to avoid a crash correlated with scanning the whole
   address space in-process -- a later, separate step if wanted. */
uint32_t CE_CALL CEAdapterRenameSecondGalaxySystems(uint32_t second_galaxy_ptr) {
    uint32_t con_list, con_array, con_count, index;
    uint32_t renamed = 0u;
    const uint32_t pool_size = (uint32_t)(
        sizeof(g_ce_second_system_names) / sizeof(g_ce_second_system_names[0]));

    if (second_galaxy_ptr == 0u ||
        !ce_region_has_access((const void *)(uintptr_t)(second_galaxy_ptr + 0x2cu), 4u, 0)) {
        return 0u;
    }
    con_list = *(const uint32_t *)(uintptr_t)(second_galaxy_ptr + 0x2cu);
    if (!ce_read_plain_list(con_list, &con_array, &con_count)) return 0u;

    for (index = 0; index < con_count; ++index) {
        uint32_t object = *(const uint32_t *)(uintptr_t)(con_array + index * 4u);
        uint32_t new_name;
        if (!ce_region_has_access((const void *)(uintptr_t)object, 0x14u, 1)) continue;
        new_name = ce_make_owned_widestring(g_ce_second_system_names[index % pool_size]);
        if (new_name == 0u) continue;
        *(uint32_t *)(uintptr_t)(object + 0x10u) = new_name;
        ++renamed;
    }
    return renamed;
}

uint32_t CE_CALL CEAdapterSetRememberedShipStar(uint32_t star_ptr) {
    InterlockedExchange(&g_ce_remembered_ship_star, (LONG)star_ptr);
    return 1u;
}

uint32_t CE_CALL CEAdapterGetRememberedShipStar(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_remembered_ship_star, 0, 0);
}

/* See CE_RVA_STAR_LOOKUP_RAISE_CALL's own comment: this replaces the call
   to System.@RaiseExcept at the exact point the "find star by id" lookup
   (VA 0x00841e60) gives up, with a small stub that logs and returns
   normally instead. The lookup's own local result slot is already 0 at
   that point (initialized before its search loop, only ever overwritten
   on a successful match), so simply not raising and falling through to
   the function's existing epilogue reproduces the same "not found" result
   every caller must already tolerate -- nothing else about the function
   changes. PUSHAD/POPAD keeps this maximally safe: no assumption about
   which registers the surrounding code still needs live. */
__attribute__((used)) static void CE_CALL ce_star_lookup_recover_body(void) {
    InterlockedIncrement(&g_ce_star_lookup_recovered_count);
    ce_write_progress("star-lookup:recovered-not-found");
}

#if defined(__i386__)
__attribute__((naked)) static void ce_star_lookup_recover_hook(void) {
    __asm__ volatile(
        "pushal\n\t"
        "call _ce_star_lookup_recover_body\n\t"
        "popal\n\t"
        "ret\n\t"
    );
}
#endif

#if defined(__i386__)
/* Shared by every "redirect this specific `call System.@RaiseExcept` to a
   log-and-fall-through stub instead" patch in this file -- same signature
   check, same VirtualProtect/patch/flush dance, just parameterized on
   which 5-byte call site and which flag/hook to use. */
static uint32_t ce_install_raise_recovery_patch(
    uint32_t galaxy_ptr, uintptr_t target_rva, void (*hook)(void),
    volatile LONG *installed_flag
) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[5];
    DWORD old_protect;
    DWORD ignored_protect;
    int32_t relative;
    LONG hook_state;

    hook_state = InterlockedCompareExchange(installed_flag, -1, 0);
    if (hook_state == 1) return 1u;
    if (hook_state != 0) return 0u;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        InterlockedExchange(installed_flag, 0);
        return 0;
    }
    target = (unsigned char *)(module_base + target_rva);
    if (target[0] != 0xe8) {
        InterlockedExchange(installed_flag, 0);
        return 0;
    }
    patch[0] = 0xe8;
    relative = (int32_t)((unsigned char *)(uintptr_t)hook - (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    if (!VirtualProtect(target, 5u, PAGE_EXECUTE_READWRITE, &old_protect)) {
        InterlockedExchange(installed_flag, 0);
        return 0;
    }
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, 5u, old_protect, &ignored_protect);
    InterlockedExchange(installed_flag, 1);
    return 1;
}
#endif

uint32_t CE_CALL CEAdapterInstallStarLookupRecovery(uint32_t galaxy_ptr) {
#if defined(__i386__)
    return ce_install_raise_recovery_patch(galaxy_ptr, CE_RVA_STAR_LOOKUP_RAISE_CALL,
        ce_star_lookup_recover_hook, &g_ce_star_lookup_hook_installed);
#else
    (void)galaxy_ptr;
    return 0;
#endif
}

/* The MUCH more impactful fix, found by re-disassembling from this exact
   function's start rather than just the reported address: VA 0x0072f89e
   is not a standalone check -- it is the `except` handler of a try/finally
   wrapped directly around `call TGalaxy.NextDay` (VA 0x00840f08, confirmed
   by the disassembly right above it: `mov eax,[0x88263c]; mov eax,[eax];
   call 0x840f08`) plus one post-processing call (VA 0x00841be8). Every
   single day-skip in the entire game runs through this one function, and
   its handler just builds a message and re-raises -- there is no higher
   catch for it, so ANY exception thrown anywhere inside that day's
   NextDay (not only the one specific "star not found" case already
   patched above) surfaces as this exact same "ThCa label = 1" dialog and
   kills the process. Confirmed empirically: patching only the star-lookup
   raise did not stop this crash from recurring, meaning a different,
   still-unidentified fault inside NextDay's own processing was reaching
   this outer handler instead. Redirecting THIS raise is a strictly more
   general fix -- the handler's own success path (VA 0x0072f7e3) already
   jumps past this whole block to the identical epilogue at 0x0072f8a8,
   so falling through here after logging reproduces exactly what a clean
   day-skip already does, regardless of which inner check actually failed. */
uint32_t CE_CALL CEAdapterInstallDayProcessRecovery(uint32_t galaxy_ptr) {
#if defined(__i386__)
    return ce_install_raise_recovery_patch(galaxy_ptr, CE_RVA_DAY_PROCESS_RAISE_CALL,
        ce_star_lookup_recover_hook, &g_ce_day_process_hook_installed);
#else
    (void)galaxy_ptr;
    return 0;
#endif
}

/* Own hook/body (rather than reusing ce_star_lookup_recover_hook) so the
   log clearly identifies WHICH of the three patched raise sites actually
   fired -- useful now that we have three independent classes of masked
   check, and the user has separately reported other symptoms (an
   apparent date rollback, stale station UI after a swap) that may or may
   not correlate with this specific one tripping. */
__attribute__((used)) static void CE_CALL ce_nextday_label3_recover_body(void) {
    InterlockedIncrement(&g_ce_nextday_label3_recovered_count);
    ce_write_progress("nextday-label3:recovered");
}

#if defined(__i386__)
__attribute__((naked)) static void ce_nextday_label3_recover_hook(void) {
    __asm__ volatile(
        "pushal\n\t"
        "call _ce_nextday_label3_recover_body\n\t"
        "popal\n\t"
        "ret\n\t"
    );
}
#endif

/* See CE_RVA_NEXTDAY_LABEL3_RAISE_CALL's own comment. */
uint32_t CE_CALL CEAdapterInstallNextDayLabel3Recovery(uint32_t galaxy_ptr) {
#if defined(__i386__)
    return ce_install_raise_recovery_patch(galaxy_ptr, CE_RVA_NEXTDAY_LABEL3_RAISE_CALL,
        ce_nextday_label3_recover_hook, &g_ce_nextday_label3_hook_installed);
#else
    (void)galaxy_ptr;
    return 0;
#endif
}

/* See CE_RVA_DAY_COUNTER_CHECK's own comment. Unlike the three raise-call
   patches above, this is not a CALL site with a safe "just fall through"
   continuation -- it is a conditional branch that needs to land on one of
   two different existing addresses depending on a runtime check, so it
   gets its own hook shape (and its own installer below) instead of reusing
   ce_install_raise_recovery_patch. Absolute VAs are hardcoded directly
   (rather than computed from module_base) because ce_resolve_engine_galaxy
   has already confirmed, via the PE timestamp/size/signature checks, that
   this process really is running the one pinned build these were measured
   against -- module_base is 0x00400000 whenever this ever actually runs. */
__attribute__((used)) static void CE_CALL ce_day_counter_recover_body(void) {
    InterlockedIncrement(&g_ce_day_counter_recovered_count);
    ce_write_progress("day-counter:recovered-null-galaxy");
}

#if defined(__i386__)
__attribute__((naked)) static void ce_day_counter_recover_hook(void) {
    __asm__ volatile(
        "movl 0x88263c, %eax\n\t"
        "movl (%eax), %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "pushal\n\t"
        "call _ce_day_counter_recover_body\n\t"
        "popal\n\t"
        "movl $0x0072fb82, %eax\n\t"
        "jmp *%eax\n\t"
        "1:\n\t"
        "cmpl $0x12c, 0x4c(%eax)\n\t"
        "jge 2f\n\t"
        "movl $0x0072fb4c, %eax\n\t"
        "jmp *%eax\n\t"
        "2:\n\t"
        "movl $0x0072fb82, %eax\n\t"
        "jmp *%eax\n\t"
    );
}

/* Patches the full 16-byte sequence (mov+mov+cmp+jge) with a 5-byte JMP to
   the hook above, padding the remaining 11 bytes with NOP -- nothing ever
   falls through into the padding since the hook always jumps away, but
   leaving well-formed single-byte NOPs instead of raw leftover opcode
   fragments keeps anything that ever disassembles this region again (this
   tool included) from misdecoding it as bogus instructions. Verifies the
   FULL original byte sequence first (not just the first byte, unlike the
   simpler CALL patches) since this signature is more distinctive and the
   cost of matching it exactly is low. */
uint32_t CE_CALL CEAdapterInstallDayCounterGuard(uint32_t galaxy_ptr) {
    static const unsigned char expected[16] = {
        0xa1, 0x3c, 0x26, 0x88, 0x00, 0x8b, 0x00, 0x81,
        0x78, 0x4c, 0x2c, 0x01, 0x00, 0x00, 0x7d, 0x36
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[16];
    DWORD old_protect;
    DWORD ignored_protect;
    int32_t relative;
    LONG hook_state;

    hook_state = InterlockedCompareExchange(&g_ce_day_counter_hook_installed, -1, 0);
    if (hook_state == 1) return 1u;
    if (hook_state != 0) return 0u;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        InterlockedExchange(&g_ce_day_counter_hook_installed, 0);
        return 0;
    }
    target = (unsigned char *)(module_base + CE_RVA_DAY_COUNTER_CHECK);
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        InterlockedExchange(&g_ce_day_counter_hook_installed, 0);
        return 0;
    }
    patch[0] = 0xe9;
    relative = (int32_t)((unsigned char *)(uintptr_t)ce_day_counter_recover_hook - (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    memset(patch + 5u, 0x90, sizeof(patch) - 5u);
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        InterlockedExchange(&g_ce_day_counter_hook_installed, 0);
        return 0;
    }
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old_protect, &ignored_protect);
    InterlockedExchange(&g_ce_day_counter_hook_installed, 1);
    return 1;
}
#else
uint32_t CE_CALL CEAdapterInstallDayCounterGuard(uint32_t galaxy_ptr) {
    (void)galaxy_ptr;
    return 0;
}
#endif

/* See CE_RVA_LOADGAME_RAISE_CANNOT_OPEN's comment. Records which LoadGame
   validation rejected the save, plus the filename LoadGame itself was given
   (read out of its own still-live frame), then falls through to the real
   raise so the engine's behaviour is unchanged. */
__attribute__((used)) static void CE_CALL ce_loadgame_diag_body(
    uint32_t reason, uint32_t filename
) {
    static const char *const reasons[5] = {
        "unknown", "cannot-open-file", "bad-pre-signature",
        "bad-post-signature", "compressed-galaxy-read-fail"
    };
    char payload[640];
    char name[256];
    size_t used = 0u;
    int size;
    name[0] = '\0';
    if (filename >= 0x10000u &&
            ce_region_has_access((const void *)(uintptr_t)(filename - 4u), 4u, 0)) {
        uint32_t bytes = *(const uint32_t *)(uintptr_t)(filename - 4u);
        if ((bytes & 1u) == 0u && bytes != 0u && bytes < 4096u &&
                ce_region_has_access((const void *)(uintptr_t)filename, bytes, 0)) {
            const wchar_t *text = (const wchar_t *)(uintptr_t)filename;
            size_t count = (size_t)bytes / sizeof(wchar_t);
            size_t index;
            /* Escaped ASCII only: the path is Unicode and this log is read
               as plain text, so anything non-ASCII becomes \uXXXX rather
               than mojibake that would have to be decoded again later. */
            for (index = 0u; index < count && used + 8u < sizeof(name); ++index) {
                wchar_t ch = text[index];
                if (ch == L'\\') { name[used++] = '/'; }
                else if (ch >= 0x20 && ch < 0x7f) { name[used++] = (char)ch; }
                else {
                    int written = snprintf(name + used, sizeof(name) - used,
                        "\\u%04x", (unsigned)ch);
                    if (written > 0) used += (size_t)written;
                }
            }
            name[used] = '\0';
        }
    }
    size = snprintf(payload, sizeof(payload),
        "{\"status\":\"loadgame-failed\",\"reason\":\"%s\",\"file\":\"%s\"}\r\n",
        reasons[reason < 5u ? reason : 0u], name);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
}

#if defined(__i386__)
/* EBP still belongs to LoadGame here, so [ebp-4] is its filename argument.
   The absolute RaiseExcept address is safe to hardcode for the same reason
   the day-counter guard does it: ce_resolve_engine_galaxy has already
   confirmed the pinned build, whose image base is always 0x00400000. */
#define CE_LOADGAME_DIAG_STUB(name, id)                    \
    __attribute__((naked)) static void name(void) {        \
        __asm__ volatile(                                  \
            "pushal\n\t"                                   \
            "pushfl\n\t"                                   \
            "movl -4(%%ebp), %%eax\n\t"                    \
            "pushl %%eax\n\t"                              \
            "pushl $" #id "\n\t"                           \
            "call _ce_loadgame_diag_body\n\t"              \
            "addl $8, %%esp\n\t"                           \
            "popfl\n\t"                                    \
            "popal\n\t"                                    \
            "movl $0x00404e20, %%eax\n\t"                  \
            "jmp *%%eax\n\t"                               \
            ::: "memory");                                 \
    }

CE_LOADGAME_DIAG_STUB(ce_loadgame_diag_cannot_open, 1)
CE_LOADGAME_DIAG_STUB(ce_loadgame_diag_bad_pre_sig, 2)
CE_LOADGAME_DIAG_STUB(ce_loadgame_diag_bad_post_sig, 3)
CE_LOADGAME_DIAG_STUB(ce_loadgame_diag_galaxy_read, 4)

/* Records LoadGame's own boolean result plus the galaxy pointer the engine
   is left holding, so a silent clean exit can be attributed to either the
   load itself failing or to whatever runs after a successful one. */
__attribute__((used)) static void CE_CALL ce_loadgame_result_body(uint32_t result) {
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    uint32_t galaxy = 0u;
    LONG pending;
    int queued = 0;
    unsigned char previous_form = 0xffu;
    char payload[224];
    int size;
    if (ce_region_has_access(
            (const void *)(module_base + CE_RVA_GALAXY_IMPORT_CELL), 4u, 0)) {
        uint32_t cell = *(const uint32_t *)(module_base + CE_RVA_GALAXY_IMPORT_CELL);
        if (ce_region_has_access((const void *)(uintptr_t)cell, 4u, 0)) {
            galaxy = *(const uint32_t *)(uintptr_t)cell;
        }
    }
    /* See CE_RVA_FORM_NEXT_CELL: a load the player started from inside the
       game leaves nothing queued, so the engine ends its message loop and
       halts cleanly. Queue StarMap ourselves, but only for this mod's own
       portal load -- a normal menu load already queues its own follow-up
       and must not be touched. */
    pending = InterlockedExchange(&g_ce_portal_load_pending, 0);
    if ((result & 0xffu) != 0u && pending != 0 &&
            ce_region_has_access(
                (const void *)(module_base + CE_RVA_FORM_NEXT_CELL), 4u, 0)) {
        uint32_t slot = *(const uint32_t *)(module_base + CE_RVA_FORM_NEXT_CELL);
        if (ce_region_has_access((void *)(uintptr_t)slot, 1u, 1)) {
            previous_form = *(const unsigned char *)(uintptr_t)slot;
            *(unsigned char *)(uintptr_t)slot = (unsigned char)CE_FORM_INDEX_STARMAP;
            queued = 1;
        }
    }
    size = snprintf(payload, sizeof(payload),
        "{\"status\":\"loadgame-result\",\"ok\":%lu,\"galaxy\":%lu,"
        "\"portal_pending\":%ld,\"queued_starmap\":%d,\"previous_form\":%d,"
        "\"tick\":%lu}\r\n",
        (unsigned long)(result & 0xffu), (unsigned long)galaxy, (long)pending,
        queued, (int)(signed char)previous_form, (unsigned long)GetTickCount());
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
}

/* EAX already holds LoadGame's filename argument when this replaces the
   original call, so the stub just forwards it, keeps the returned AL, and
   logs around it. ECX/EDX are free: LoadGame takes a single register
   argument. */
__attribute__((naked)) static void ce_loadgame_result_hook(void) {
    __asm__ volatile(
        "movl $0x00601150, %ecx\n\t"
        "call *%ecx\n\t"
        "pushl %eax\n\t"
        "movzbl %al, %eax\n\t"
        "pushl %eax\n\t"
        "call _ce_loadgame_result_body\n\t"
        "addl $4, %esp\n\t"
        "popl %eax\n\t"
        "ret\n\t"
    );
}

static int ce_patch_loadgame_call(
    uintptr_t module_base, uintptr_t target_rva, void (*hook)(void)
) {
    unsigned char *target = (unsigned char *)(module_base + target_rva);
    unsigned char patch[5];
    DWORD old_protect;
    DWORD ignored_protect;
    int32_t relative;
    if (target[0] != 0xe8) return 0;
    patch[0] = 0xe8;
    relative = (int32_t)((unsigned char *)(uintptr_t)hook - (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return 0;
    }
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old_protect, &ignored_protect);
    return 1;
}

uint32_t CE_CALL CEAdapterInstallLoadGameDiagnostics(uint32_t galaxy_ptr) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    int installed = 0;
    LONG state = InterlockedCompareExchange(&g_ce_loadgame_diag_installed, -1, 0);
    if (state == 1) return 1u;
    if (state != 0) return 0u;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        InterlockedExchange(&g_ce_loadgame_diag_installed, 0);
        return 0;
    }
    installed += ce_patch_loadgame_call(module_base,
        CE_RVA_LOADGAME_RAISE_CANNOT_OPEN, ce_loadgame_diag_cannot_open);
    installed += ce_patch_loadgame_call(module_base,
        CE_RVA_LOADGAME_RAISE_BAD_PRE_SIG, ce_loadgame_diag_bad_pre_sig);
    installed += ce_patch_loadgame_call(module_base,
        CE_RVA_LOADGAME_RAISE_BAD_POST_SIG, ce_loadgame_diag_bad_post_sig);
    installed += ce_patch_loadgame_call(module_base,
        CE_RVA_LOADGAME_RAISE_GALAXY_READ, ce_loadgame_diag_galaxy_read);
    installed += ce_patch_loadgame_call(module_base,
        CE_RVA_LOADGAME_CALL_IN_THREAD, ce_loadgame_result_hook);
    /* The form-gate patch that used to go here is gone: it never fired in a
       live transit, and its host function turned out to pick music, not
       forms. Queueing GFormNext from the result hook replaces it. */
    InterlockedExchange(&g_ce_loadgame_diag_installed, installed == 5 ? 1 : 0);
    return installed == 5 ? 1u : 0u;
}
#else
uint32_t CE_CALL CEAdapterInstallLoadGameDiagnostics(uint32_t galaxy_ptr) {
    (void)galaxy_ptr;
    return 0;
}
#endif

/* See g_ce_player_stash. Slot layout is owned by the RScript side; the
   adapter only keeps the numbers alive across LoadGame. */
uint32_t CE_CALL CEAdapterStashPlayerValue(uint32_t slot, uint32_t value) {
    if (slot >= CE_PLAYER_STASH_SLOTS) return 0u;
    InterlockedExchange(&g_ce_player_stash[slot], (LONG)value);
    InterlockedExchange(&g_ce_player_stash_ready, 1);
    return 1u;
}

uint32_t CE_CALL CEAdapterFetchPlayerValue(uint32_t slot) {
    if (slot >= CE_PLAYER_STASH_SLOTS) return 0u;
    return (uint32_t)InterlockedCompareExchange(&g_ce_player_stash[slot], 0, 0);
}

uint32_t CE_CALL CEAdapterPlayerStashReady(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_player_stash_ready, 0, 0);
}

uint32_t CE_CALL CEAdapterClearPlayerStash(void) {
    InterlockedExchange(&g_ce_player_stash_ready, 0);
    return 1u;
}

/* DLL globals survive LoadGame but not a new process, and the transition may
   yet move to launching the second arm as its own process -- which is the
   one route that removes the engine-lifecycle failures entirely, since each
   arm then loads through the engine's ordinary startup path instead of a
   mid-game LoadGame. Persisting the traveller's state to disk works for both
   routes, so it is worth having regardless of which one wins. Deliberately
   plain: a magic word, the two array shapes, then the values and the items.
   The shapes are in the header so a file written by an older build is
   rejected outright rather than read as garbage -- the item record grew from
   four fields to eight, and a silent misread would rebuild the traveller's
   gear from shifted numbers. */
enum { CE_PLAYER_STASH_MAGIC = 0x32534543u }; /* "CES2" */

static const char *ce_player_stash_path(void) {
    return "C:\\ce_debug\\ce_player_stash.bin";
}

uint32_t CE_CALL CEAdapterSavePlayerStash(void) {
    uint32_t header[4];
    uint32_t values[CE_PLAYER_STASH_SLOTS];
    uint32_t items[CE_ITEM_STASH_MAX * CE_ITEM_STASH_FIELDS];
    uint32_t index;
    uint32_t field;
    LONG item_count;
    HANDLE file;
    DWORD written = 0;
    if (InterlockedCompareExchange(&g_ce_player_stash_ready, 0, 0) == 0) return 0u;
    item_count = InterlockedCompareExchange(&g_ce_item_stash_count, 0, 0);
    if (item_count < 0) item_count = 0;
    if ((uint32_t)item_count > CE_ITEM_STASH_MAX) item_count = (LONG)CE_ITEM_STASH_MAX;
    header[0] = CE_PLAYER_STASH_MAGIC;
    header[1] = CE_PLAYER_STASH_SLOTS;
    header[2] = CE_ITEM_STASH_FIELDS;
    header[3] = (uint32_t)item_count;
    for (index = 0u; index < CE_PLAYER_STASH_SLOTS; ++index) {
        values[index] = (uint32_t)InterlockedCompareExchange(
            &g_ce_player_stash[index], 0, 0);
    }
    for (index = 0u; index < (uint32_t)item_count; ++index) {
        for (field = 0u; field < CE_ITEM_STASH_FIELDS; ++field) {
            items[index * CE_ITEM_STASH_FIELDS + field] =
                (uint32_t)InterlockedCompareExchange(
                    &g_ce_item_stash[index][field], 0, 0);
        }
    }
    CreateDirectoryA("C:\\ce_debug", NULL);
    file = CreateFileA(ce_player_stash_path(), GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0u;
    WriteFile(file, header, (DWORD)sizeof(header), &written, NULL);
    WriteFile(file, values, (DWORD)sizeof(values), &written, NULL);
    if (item_count > 0) {
        WriteFile(file, items,
            (DWORD)((uint32_t)item_count * CE_ITEM_STASH_FIELDS * sizeof(uint32_t)),
            &written, NULL);
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    ce_write_progress("player-stash:saved");
    return 1u;
}

uint32_t CE_CALL CEAdapterLoadPlayerStash(void) {
    uint32_t header[4];
    uint32_t values[CE_PLAYER_STASH_SLOTS];
    uint32_t items[CE_ITEM_STASH_MAX * CE_ITEM_STASH_FIELDS];
    uint32_t index;
    uint32_t field;
    uint32_t item_bytes;
    HANDLE file;
    DWORD got = 0;
    file = CreateFileA(ce_player_stash_path(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0u;
    if (!ReadFile(file, header, (DWORD)sizeof(header), &got, NULL) ||
            got != sizeof(header) || header[0] != CE_PLAYER_STASH_MAGIC ||
            header[1] != CE_PLAYER_STASH_SLOTS ||
            header[2] != CE_ITEM_STASH_FIELDS ||
            header[3] > CE_ITEM_STASH_MAX) {
        CloseHandle(file);
        ce_write_progress("player-stash:load-rejected");
        return 0u;
    }
    if (!ReadFile(file, values, (DWORD)sizeof(values), &got, NULL) ||
            got != sizeof(values)) {
        CloseHandle(file);
        ce_write_progress("player-stash:load-truncated");
        return 0u;
    }
    item_bytes = header[3] * CE_ITEM_STASH_FIELDS * (uint32_t)sizeof(uint32_t);
    if (item_bytes > 0u &&
            (!ReadFile(file, items, (DWORD)item_bytes, &got, NULL) ||
             got != item_bytes)) {
        CloseHandle(file);
        ce_write_progress("player-stash:load-items-truncated");
        return 0u;
    }
    CloseHandle(file);
    for (index = 0u; index < CE_PLAYER_STASH_SLOTS; ++index) {
        InterlockedExchange(&g_ce_player_stash[index], (LONG)values[index]);
    }
    for (index = 0u; index < header[3]; ++index) {
        for (field = 0u; field < CE_ITEM_STASH_FIELDS; ++field) {
            InterlockedExchange(&g_ce_item_stash[index][field],
                (LONG)items[index * CE_ITEM_STASH_FIELDS + field]);
        }
    }
    InterlockedExchange(&g_ce_item_stash_count, (LONG)header[3]);
    InterlockedExchange(&g_ce_player_stash_ready, 1);
    ce_write_progress("player-stash:loaded");
    return 1u;
}

#if defined(__i386__)
/* See CE_RVA_POST_NEXTDAY_PASS. Steals the 6-byte prologue
   (push ebp; mov ebp,esp; add esp,-0x1c) and re-emits it after the check. */
__attribute__((used)) static void CE_CALL ce_post_nextday_null_body(void) {
    ce_write_progress("post-nextday:skipped-null-galaxy");
}

__attribute__((naked)) static void ce_post_nextday_guard_hook(void) {
    __asm__ volatile(
        "testl %eax, %eax\n\t"
        "jz 1f\n\t"
        "pushl %ebp\n\t"
        "movl %esp, %ebp\n\t"
        "addl $-0x1c, %esp\n\t"
        "movl $0x00841bee, %ecx\n\t"
        "jmp *%ecx\n\t"
        "1:\n\t"
        "pushal\n\t"
        "call _ce_post_nextday_null_body\n\t"
        "popal\n\t"
        "ret\n\t"
    );
}

uint32_t CE_CALL CEAdapterInstallPostNextDayGuard(uint32_t galaxy_ptr) {
    static const unsigned char expected[6] = { 0x55, 0x8b, 0xec, 0x83, 0xc4, 0xe4 };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    unsigned char patch[6];
    DWORD old_protect, ignored_protect;
    int32_t relative;
    LONG state = InterlockedCompareExchange(&g_ce_post_nextday_guard_installed, -1, 0);
    if (state == 1) return 1u;
    if (state != 0) return 0u;
    if (!ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        InterlockedExchange(&g_ce_post_nextday_guard_installed, 0);
        return 0u;
    }
    target = (unsigned char *)(module_base + CE_RVA_POST_NEXTDAY_PASS);
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        InterlockedExchange(&g_ce_post_nextday_guard_installed, 0);
        return 0u;
    }
    patch[0] = 0xe9;
    relative = (int32_t)((unsigned char *)(uintptr_t)ce_post_nextday_guard_hook - (target + 5u));
    memcpy(patch + 1u, &relative, sizeof(relative));
    patch[5] = 0x90;
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        InterlockedExchange(&g_ce_post_nextday_guard_installed, 0);
        return 0u;
    }
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old_protect, &ignored_protect);
    InterlockedExchange(&g_ce_post_nextday_guard_installed, 1);
    return 1u;
}
#else
uint32_t CE_CALL CEAdapterInstallPostNextDayGuard(uint32_t galaxy_ptr) {
    (void)galaxy_ptr;
    return 0u;
}
#endif

/* See CE_RVA_SECTOR_COUNT_IMMEDIATE. Rewrites the compiled-in sector count
   so the next galaxy the engine builds gets more constellations. Refuses
   counts the name pool cannot cover -- Constellations.Name currently ends at
   44, and unnamed sectors are worse than fewer sectors. */
uint32_t CE_CALL CEAdapterSetSectorCount(uint32_t galaxy_ptr, uint32_t count) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    unsigned char *target;
    DWORD old_protect, ignored_protect;
    uint32_t previous;
    char report[128];
    int size;

    if (count < 1u || count > 44u) return 0u;
    if (!ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0u;
    }
    target = (unsigned char *)(module_base + CE_RVA_SECTOR_COUNT_IMMEDIATE);
    memcpy(&previous, target, sizeof(previous));
    if (previous == count) return 1u;
    /* Only ever rewrite what still looks like the stock value or a value this
       function itself wrote; anything else means the site moved. */
    if (previous < 1u || previous > 44u) return 0u;
    if (!VirtualProtect(target, sizeof(count), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return 0u;
    }
    memcpy(target, &count, sizeof(count));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(count));
    VirtualProtect(target, sizeof(count), old_protect, &ignored_protect);
    size = snprintf(report, sizeof(report),
        "{\"status\":\"sector-count-patched\",\"from\":%lu,\"to\":%lu}\r\n",
        (unsigned long)previous, (unsigned long)count);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", report, (size_t)size);
    return 1u;
}

/* See CE_GALAXY_TURN_FIELD. Only accepts values in a sane range and only
   moves the date forward: a save is allowed to catch up with the traveller,
   never to be rewound, since rewinding would make already-processed days
   happen twice. */
uint32_t CE_CALL CEAdapterSetGalaxyTurn(uint32_t galaxy_ptr, uint32_t turn) {
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t previous;
    char report[144];
    int size;

    if (turn == 0u || turn > 1000000u) return 0u;
    if (!ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref)) {
        return 0u;
    }
    if (!ce_region_has_access(
            (void *)(uintptr_t)(galaxy_ptr + CE_GALAXY_TURN_FIELD), 4u, 1)) {
        return 0u;
    }
    previous = *(const uint32_t *)(uintptr_t)(galaxy_ptr + CE_GALAXY_TURN_FIELD);
    if (turn <= previous) return 1u;
    *(uint32_t *)(uintptr_t)(galaxy_ptr + CE_GALAXY_TURN_FIELD) = turn;
    size = snprintf(report, sizeof(report),
        "{\"status\":\"galaxy-turn-synced\",\"from\":%lu,\"to\":%lu}\r\n",
        (unsigned long)previous, (unsigned long)turn);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", report, (size_t)size);
    return 1u;
}

/* See g_ce_item_stash. Begin clears the list so a second transit does not
   inherit the previous one's cargo. */
/* Re-enter the star map without advancing a day.

   Everything the traveller carries is applied while the engine is still
   inside the form-enter call that follows the load, so the map on screen was
   already drawn from the values the arriving save had, and only redrew when
   something else forced it -- which in practice meant skipping a day. Asking
   through the script FormChange() does not help: it sees the target form is
   the one already showing and takes a shorter path that sets a flag instead
   of queueing an entry.

   The main loop wants one byte. Written directly, the next pass through it
   calls the star map's enter method again and clears the byte itself, so this
   costs one extra rebuild and leaves nothing behind. */
uint32_t CE_CALL CEAdapterRequestStarMapReenter(void) {
    uintptr_t module_base = (uintptr_t)GetModuleHandleW(NULL);
    uint32_t slot;
    unsigned char before = 0xffu;
    int entering = -1;
    int settled = -1;
    int written = 0;
    char payload[192];
    int size;
    if (module_base == 0) return 0u;
    if (!ce_region_has_access(
            (const void *)(module_base + CE_RVA_FORM_NEXT_CELL), 4u, 0)) return 0u;
    slot = *(const uint32_t *)(module_base + CE_RVA_FORM_NEXT_CELL);
    if (!ce_region_has_access((void *)(uintptr_t)slot, 1u, 1)) return 0u;
    before = *(const unsigned char *)(uintptr_t)slot;
    /* A non-zero byte means the engine has already queued a form of its own and
       has not reached it yet. Overwriting that is how the return leg ended up
       showing the map while the game still had the player docked: the engine
       was on its way to another form and this quietly cancelled it. An
       already-queued transition rebuilds the screen anyway, so there is
       nothing to add -- leave it alone and say so in the log. */
    if (before == 0u) {
        *(unsigned char *)(uintptr_t)slot = (unsigned char)CE_FORM_INDEX_STARMAP;
        written = 1;
    }
    if (ce_region_has_access(
            (const void *)(module_base + CE_RVA_FORM_ENTERING_CELL), 4u, 0)) {
        uint32_t cell = *(const uint32_t *)(module_base + CE_RVA_FORM_ENTERING_CELL);
        if (ce_region_has_access((const void *)(uintptr_t)cell, 1u, 0)) {
            entering = (int)*(const unsigned char *)(uintptr_t)cell;
        }
    }
    if (ce_region_has_access(
            (const void *)(module_base + CE_RVA_FORM_SETTLED_CELL), 4u, 0)) {
        uint32_t cell = *(const uint32_t *)(module_base + CE_RVA_FORM_SETTLED_CELL);
        if (ce_region_has_access((const void *)(uintptr_t)cell, 1u, 0)) {
            settled = (int)*(const unsigned char *)(uintptr_t)cell;
        }
    }
    size = snprintf(payload, sizeof(payload),
        "{\"status\":\"starmap-reenter\",\"pending_before\":%d,"
        "\"entering\":%d,\"settled\":%d,\"written\":%d}\r\n",
        (int)before, entering, settled, written);
    if (size > 0) ce_write_text_marker("live-arm-switch.jsonl", payload, (size_t)size);
    return (uint32_t)written;
}

uint32_t CE_CALL CEAdapterStashItemsBegin(void) {
    InterlockedExchange(&g_ce_item_stash_count, 0);
    return 1u;
}

uint32_t CE_CALL CEAdapterStashItem(
    uint32_t type, uint32_t size, uint32_t level, uint32_t owner,
    uint32_t slot, uint32_t wear, uint32_t special, uint32_t module,
    uint32_t hull_class
) {
    LONG index = InterlockedCompareExchange(&g_ce_item_stash_count, 0, 0);
    if (index < 0 || (uint32_t)index >= CE_ITEM_STASH_MAX) return 0u;
    InterlockedExchange(&g_ce_item_stash[index][0], (LONG)type);
    InterlockedExchange(&g_ce_item_stash[index][1], (LONG)size);
    InterlockedExchange(&g_ce_item_stash[index][2], (LONG)level);
    InterlockedExchange(&g_ce_item_stash[index][3], (LONG)owner);
    InterlockedExchange(&g_ce_item_stash[index][4], (LONG)slot);
    InterlockedExchange(&g_ce_item_stash[index][5], (LONG)wear);
    InterlockedExchange(&g_ce_item_stash[index][6], (LONG)special);
    InterlockedExchange(&g_ce_item_stash[index][7], (LONG)module);
    InterlockedExchange(&g_ce_item_stash[index][8], (LONG)hull_class);
    InterlockedExchange(&g_ce_item_stash_count, index + 1);
    return 1u;
}

uint32_t CE_CALL CEAdapterStashedItemCount(void) {
    return (uint32_t)InterlockedCompareExchange(&g_ce_item_stash_count, 0, 0);
}

uint32_t CE_CALL CEAdapterStashedItemField(uint32_t index, uint32_t field) {
    if (index >= CE_ITEM_STASH_MAX || field >= CE_ITEM_STASH_FIELDS) return 0u;
    return (uint32_t)InterlockedCompareExchange(&g_ce_item_stash[index][field], 0, 0);
}

uint32_t CE_CALL CEAdapterSnapshotGalaxy(uint32_t galaxy_ptr) {
    static const unsigned char save_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xa0, 0x33, 0xc9,
        0x89, 0x4d, 0xa0, 0x89, 0x55, 0xf8, 0x89, 0x45
    };
    static const unsigned char buffer_ctor_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xf8, 0x84, 0xd2,
        0x74, 0x08, 0x83, 0xc4, 0xf0, 0xe8
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t buffer_class_ref;
    uint32_t buffer = 0;
    uint32_t length;
    uint32_t capacity;
    uint32_t position;
    uint32_t data;
    uint32_t hash;
    char temp_path[MAX_PATH];
    char marker_dir[MAX_PATH];
    char report[192];
    int report_size;
    int wrote_temp;

    if (InterlockedCompareExchange(&g_ce_snapshot_done, 0, 0) != 0) return 2;
    if (InterlockedCompareExchange(&g_ce_snapshot_lock, 1, 0) != 0) return 0;
    if (!ce_resolve_engine_galaxy(
            galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref) ||
        memcmp((const void *)(module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM),
            save_signature, sizeof(save_signature)) != 0 ||
        memcmp((const void *)(module_base + CE_RVA_BUFFER_CONSTRUCTOR),
            buffer_ctor_signature, sizeof(buffer_ctor_signature)) != 0 ||
        !ce_region_has_access(
            (const void *)(module_base + CE_RVA_BUFFER_CLASS_CELL), 4u, 0)) {
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }

    buffer_class_ref = *(const uint32_t *)(module_base + CE_RVA_BUFFER_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)buffer_class_ref, 4u, 0)) {
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }
    buffer = ce_call_delphi_constructor(
        buffer_class_ref, module_base + CE_RVA_BUFFER_CONSTRUCTOR
    );
    if (buffer == 0 || !ce_region_has_access((const void *)(uintptr_t)buffer, 0x14u, 1)) {
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }

    ce_call_delphi_method_dword(
        galaxy_ptr, buffer, module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM
    );
    length = *(const uint32_t *)(uintptr_t)(buffer + 0x04u);
    capacity = *(const uint32_t *)(uintptr_t)(buffer + 0x08u);
    position = *(const uint32_t *)(uintptr_t)(buffer + 0x0cu);
    data = *(const uint32_t *)(uintptr_t)(buffer + 0x10u);
    if (length == 0 || length > capacity || position != length ||
        length > 256u * 1024u * 1024u ||
        !ce_region_has_access((const void *)(uintptr_t)data, length, 0)) {
        ce_call_delphi_method(buffer, module_base + CE_RVA_TOBJECT_FREE);
        InterlockedExchange(&g_ce_snapshot_lock, 0);
        return 0;
    }

    hash = ce_fnv1a32((const unsigned char *)(uintptr_t)data, length);
    ce_write_binary_file("C:\\ce_debug", "galaxy-snapshot.bin",
        (const void *)(uintptr_t)data, length);
    wrote_temp = 0;
    if (GetTempPathA(MAX_PATH, temp_path) != 0 &&
        snprintf(marker_dir, sizeof(marker_dir), "%sChildrenOfEltan", temp_path) >= 0) {
        wrote_temp = ce_write_binary_file(marker_dir, "galaxy-snapshot.bin",
            (const void *)(uintptr_t)data, length);
    }
    report_size = snprintf(report, sizeof(report),
        "{\"abi\":%u,\"length\":%u,\"position\":%u,\"capacity\":%u,"
        "\"fnv1a32\":%u,\"temp_written\":%s}\r\n",
        CEAdapterAbiVersion(), length, position, capacity, hash,
        wrote_temp ? "true" : "false");
    if (report_size > 0) {
        ce_write_text_marker("galaxy-snapshot.jsonl", report, (size_t)report_size);
    }
    ce_call_delphi_method(buffer, module_base + CE_RVA_TOBJECT_FREE);
    if (wrote_temp) InterlockedExchange(&g_ce_snapshot_done, 1);
    InterlockedExchange(&g_ce_snapshot_lock, 0);
    return wrote_temp ? 1u : 0u;
}

/* CEAdapterSnapshotGalaxy above already proves this exact shape works --
   construct a real buffer object (CE_RVA_BUFFER_CLASS_CELL/CONSTRUCTOR),
   call TGalaxy.SaveToStream on it DIRECTLY (not via the passive hook that
   only fires when the ENGINE itself happens to save), read the result,
   free the buffer -- but it is one-shot-forever (g_ce_snapshot_done) and
   completely unguarded, and every reference to "SaveToStream cannot safely
   be called from a Turn callback" in this file is about calling it from
   TURN-CODE specifically (the real deadlock repro). This is called from
   the anchor item's OWN OnUseCode instead (CE_InterarmTransit.Lang.txt,
   right before HoleCreate2) -- an interface/act-code context, the same
   category already established as safe for heavy calls throughout this
   file (OnKey, background threads) -- so it can run every time the player
   actually commits to opening a portal, capturing the ship's real current
   position/cargo instead of whatever the engine's own last autosave
   happened to contain (confirmed via telemetry: only 2 real autosaves
   fired in an entire play session). Wrapped in the same VEH+setjmp net as
   every other first-time-exercised engine call in this file, since this
   exact call path (construct-and-call, not intercept-and-observe) has
   never been exercised live before. */
uint32_t CE_CALL CEAdapterCaptureFreshSnapshot(uint32_t galaxy_ptr) {
    static const unsigned char buffer_ctor_signature[] = {
        0x55, 0x8b, 0xec, 0x83, 0xc4, 0xf8, 0x84, 0xd2,
        0x74, 0x08, 0x83, 0xc4, 0xf0, 0xe8
    };
    uintptr_t module_base;
    uint32_t *galaxy_slot;
    uint32_t unused_class_ref;
    uint32_t buffer_class_ref;
    uint32_t buffer = 0;
    uint32_t length, capacity, position, data, hash;
    char report[128];
    int report_size;
    uint32_t result = 0;

    if (InterlockedCompareExchange(&g_ce_fresh_snapshot_lock, 1, 0) != 0) return 0;
    /* Deliberately does NOT check TGalaxy.SaveToStream's own first bytes
       against their original signature (CEAdapterSnapshotGalaxy, which this
       is based on, does check that -- and would silently fail here every
       time). CEAdapterArmGalaxySaveSnapshot patches those exact 6 bytes to
       a jump into our own detour, and that arms on every single Turn-code
       tick from the very start of the game, so by the time the player ever
       gets to use the anchor item, the original bytes are already gone.
       This isn't a problem: calling this address with the detour installed
       jumps into ce_tgalaxy_save_hook, which runs the trampoline (the real,
       displaced SaveToStream body) and returns normally -- confirmed live
       this was the actual reason the very first version of this function
       never captured anything (no log line at all, since the signature
       memcmp failed before any of the real work or logging below). */
    if (!ce_resolve_engine_galaxy(galaxy_ptr, &module_base, &galaxy_slot, &unused_class_ref) ||
        memcmp((const void *)(module_base + CE_RVA_BUFFER_CONSTRUCTOR),
            buffer_ctor_signature, sizeof(buffer_ctor_signature)) != 0 ||
        !ce_region_has_access(
            (const void *)(module_base + CE_RVA_BUFFER_CLASS_CELL), 4u, 0)) {
        ce_write_progress("capture-fresh-snapshot:abort:validation-failed");
        InterlockedExchange(&g_ce_fresh_snapshot_lock, 0);
        return 0;
    }
    buffer_class_ref = *(const uint32_t *)(module_base + CE_RVA_BUFFER_CLASS_CELL);
    if (!ce_region_has_access((const void *)(uintptr_t)buffer_class_ref, 4u, 0)) {
        ce_write_progress("capture-fresh-snapshot:abort:bad-class-ref");
        InterlockedExchange(&g_ce_fresh_snapshot_lock, 0);
        return 0;
    }

    ce_ensure_veh_installed();
    if (setjmp(g_ce_recovery_point) != 0) {
        InterlockedExchange(&g_ce_guard_active, 0);
        ce_write_fault_report("capture-fresh-snapshot:FAULTED");
        InterlockedExchange(&g_ce_fresh_snapshot_lock, 0);
        return 0;
    }
    InterlockedExchange(&g_ce_guard_active, 1);

    buffer = ce_call_delphi_constructor(
        buffer_class_ref, module_base + CE_RVA_BUFFER_CONSTRUCTOR);
    if (buffer == 0 || !ce_region_has_access((const void *)(uintptr_t)buffer, 0x14u, 1)) {
        InterlockedExchange(&g_ce_guard_active, 0);
        InterlockedExchange(&g_ce_fresh_snapshot_lock, 0);
        return 0;
    }

    ce_call_delphi_method_dword(
        galaxy_ptr, buffer, module_base + CE_RVA_TGALAXY_SAVE_TO_STREAM);

    length = *(const uint32_t *)(uintptr_t)(buffer + 0x04u);
    capacity = *(const uint32_t *)(uintptr_t)(buffer + 0x08u);
    position = *(const uint32_t *)(uintptr_t)(buffer + 0x0cu);
    data = *(const uint32_t *)(uintptr_t)(buffer + 0x10u);
    if (length != 0 && length <= capacity && position == length &&
        length <= 256u * 1024u * 1024u &&
        ce_region_has_access((const void *)(uintptr_t)data, length, 0)) {
        hash = ce_fnv1a32((const unsigned char *)(uintptr_t)data, length);
        result = ce_write_binary_file("C:\\ce_debug", "galaxy-save-snapshot.bin",
            (const void *)(uintptr_t)data, length) ? 1u : 0u;
        report_size = snprintf(report, sizeof(report),
            "{\"status\":\"%s\",\"length\":%u,\"fnv1a32\":%u}\r\n",
            result ? "fresh-captured" : "fresh-write-failed", length, hash);
        if (report_size > 0) ce_write_text_marker(
            "galaxy-save-snapshot.jsonl", report, (size_t)report_size);
    }

    ce_call_delphi_method(buffer, module_base + CE_RVA_TOBJECT_FREE);
    InterlockedExchange(&g_ce_guard_active, 0);
    InterlockedExchange(&g_ce_fresh_snapshot_lock, 0);
    /* Invalidate the one-shot "already loaded" guard on the second galaxy's
       own snapshot load so the NEXT CEAdapterLoadSnapshotIntoSecondGalaxy
       call actually re-reads this fresh file instead of skipping as
       already-done -- see that function's own guard. */
    if (result) InterlockedExchange(&g_ce_second_snapshot_loaded, 0);
    return result;
}
