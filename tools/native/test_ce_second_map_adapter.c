#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef uint32_t (__cdecl *ce_noarg_fn)(void);
typedef uint32_t (__cdecl *ce_dword_fn)(uint32_t);

static FARPROC require_export(HMODULE module, const char *name) {
    FARPROC result = GetProcAddress(module, name);
    if (result == NULL) {
        fprintf(stderr, "missing export: %s\n", name);
        ExitProcess(2);
    }
    return result;
}

int main(int argc, char **argv) {
    HMODULE module;
    ce_noarg_fn abi;
    ce_noarg_fn capabilities;
    ce_noarg_fn get_bound;
    ce_noarg_fn supports_multi;
    ce_noarg_fn active_arm;
    ce_dword_fn snapshot_galaxy;
    ce_dword_fn arm_save_snapshot;
    ce_dword_fn bind;
    ce_dword_fn echo;
    typedef uint32_t (__cdecl *ce_two_dword_fn)(uint32_t, uint32_t);
    typedef uint32_t (__cdecl *ce_three_dword_fn)(uint32_t, uint32_t, uint32_t);
    ce_two_dword_fn poll_second_home;
    ce_two_dword_fn install_live_switch;
    ce_three_dword_fn set_system_sector;
    ce_dword_fn portal_ready;
    ce_noarg_fn portal_status;
    ce_two_dword_fn register_portal;
    ce_two_dword_fn enter_portal;
    ce_dword_fn complete_portal;
    ce_two_dword_fn run_smoke;
    ce_two_dword_fn probe;
    ce_two_dword_fn sample_layout;
    ce_three_dword_fn observe_layout;
    ce_dword_fn probe_engine_galaxy;
    ce_three_dword_fn create_second_galaxy;
    ce_dword_fn return_old_galaxy;
    ce_noarg_fn second_galaxy_status;
    ce_dword_fn enter_ready_second_galaxy;
    ce_noarg_fn fingerprint_hash;
    ce_noarg_fn fingerprint_bytes;
    ce_dword_fn layout_block_hash;
    ce_dword_fn layout_normalized_block_hash;
    ce_noarg_fn layout_zero_low;
    ce_noarg_fn layout_zero_high;
    ce_noarg_fn layout_pointer_low;
    ce_noarg_fn layout_pointer_high;
    ce_noarg_fn layout_sample_bytes;
    ce_noarg_fn layout_observation_count;
    union {
        uint32_t words[64];
        uint8_t bytes[256];
    } sample = {{0}};
    const uint32_t marker = 0x1234ABCDu;
    uint32_t test_index;

    if (argc != 2) {
        fprintf(stderr, "usage: test_ce_second_map_adapter.exe <dll>\n");
        return 2;
    }
    module = LoadLibraryA(argv[1]);
    if (module == NULL) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }
    abi = (ce_noarg_fn)require_export(module, "CEAdapterAbiVersion");
    capabilities = (ce_noarg_fn)require_export(module, "CEAdapterCapabilities");
    bind = (ce_dword_fn)require_export(module, "CEAdapterBindGalaxy");
    get_bound = (ce_noarg_fn)require_export(module, "CEAdapterGetBoundGalaxy");
    echo = (ce_dword_fn)require_export(module, "CEAdapterEchoDword");
    run_smoke = (ce_two_dword_fn)require_export(module, "CEAdapterRunSmoke");
    probe = (ce_two_dword_fn)require_export(module, "CEAdapterProbeGalaxy");
    fingerprint_hash = (ce_noarg_fn)require_export(module, "CEAdapterGetLastFingerprintHash");
    fingerprint_bytes = (ce_noarg_fn)require_export(module, "CEAdapterGetLastFingerprintBytes");
    sample_layout = (ce_two_dword_fn)require_export(module, "CEAdapterSampleGalaxyLayout");
    observe_layout = (ce_three_dword_fn)require_export(module, "CEAdapterObserveGalaxyLayout");
    layout_observation_count = (ce_noarg_fn)require_export(module, "CEAdapterGetLayoutObservationCount");
    layout_block_hash = (ce_dword_fn)require_export(module, "CEAdapterGetLayoutBlockHash");
    layout_normalized_block_hash = (ce_dword_fn)require_export(module, "CEAdapterGetLayoutNormalizedBlockHash");
    layout_zero_low = (ce_noarg_fn)require_export(module, "CEAdapterGetLayoutZeroMaskLow");
    layout_zero_high = (ce_noarg_fn)require_export(module, "CEAdapterGetLayoutZeroMaskHigh");
    layout_pointer_low = (ce_noarg_fn)require_export(module, "CEAdapterGetLayoutReadablePointerMaskLow");
    layout_pointer_high = (ce_noarg_fn)require_export(module, "CEAdapterGetLayoutReadablePointerMaskHigh");
    layout_sample_bytes = (ce_noarg_fn)require_export(module, "CEAdapterGetLayoutSampleBytes");
    supports_multi = (ce_noarg_fn)require_export(module, "CEAdapterSupportsNativeMultiGalaxy");
    probe_engine_galaxy = (ce_dword_fn)require_export(module, "CEAdapterProbeEngineGalaxy");
    create_second_galaxy = (ce_three_dword_fn)require_export(
        module, "CEAdapterCreateAndEnterSecondGalaxy"
    );
    second_galaxy_status = (ce_noarg_fn)require_export(module, "CEAdapterSecondGalaxyStatus");
    enter_ready_second_galaxy = (ce_dword_fn)require_export(module, "CEAdapterEnterReadySecondGalaxy");
    return_old_galaxy = (ce_dword_fn)require_export(module, "CEAdapterReturnToOldGalaxy");
    active_arm = (ce_noarg_fn)require_export(module, "CEAdapterActiveArm");
    snapshot_galaxy = (ce_dword_fn)require_export(module, "CEAdapterSnapshotGalaxy");
    arm_save_snapshot = (ce_dword_fn)require_export(module, "CEAdapterArmGalaxySaveSnapshot");
    poll_second_home = (ce_two_dword_fn)require_export(module, "CEAdapterPollSecondHomeTransform");
    install_live_switch = (ce_two_dword_fn)require_export(module, "CEAdapterInstallLiveArmSwitch");
    set_system_sector = (ce_three_dword_fn)require_export(module, "CEAdapterSetSystemSector");
    portal_ready = (ce_dword_fn)require_export(module, "CEAdapterPortalReady");
    portal_status = (ce_noarg_fn)require_export(module, "CEAdapterPortalStatus");
    register_portal = (ce_two_dword_fn)require_export(module, "CEAdapterRegisterPortal");
    enter_portal = (ce_two_dword_fn)require_export(module, "CEAdapterEnterRegisteredPortal");
    complete_portal = (ce_dword_fn)require_export(module, "CEAdapterCompleteRegisteredPortal");

    if (abi() != 15 || capabilities() != 65521 || supports_multi() != 0) return 3;
    if (probe_engine_galaxy(marker) != 0 || create_second_galaxy(marker, 1, 0) != 0 ||
        second_galaxy_status() != 0 || enter_ready_second_galaxy(marker) != 0 ||
        return_old_galaxy(marker) != 0 || active_arm() != 0 ||
        snapshot_galaxy(marker) != 0 || arm_save_snapshot(marker) != 0 ||
        poll_second_home(marker, marker) != 0 || install_live_switch(marker, marker) != 0 ||
        set_system_sector(marker, marker, marker) != 0 || portal_ready(marker) != 0 ||
        portal_status() != 0 || register_portal(marker, marker) != 0 ||
        enter_portal(marker, marker) != 0 || complete_portal(marker) != 0) return 17;
    if (echo(marker) != marker || bind(0) != 0 || bind(marker) != 1) return 4;
    if (get_bound() != marker) return 5;
    if (run_smoke(0, 1128616787u) != 0 || run_smoke(marker, 1128616787u) != 1) return 6;
    sample.bytes[0] = 0xCE;
    sample.bytes[63] = 0x31;
    if (probe((uint32_t)(uintptr_t)sample.bytes, 64) != 1) return 7;
    if (fingerprint_hash() == 0 || fingerprint_bytes() != 64) return 8;
    memset(sample.bytes, 0, sizeof(sample.bytes));
    sample.words[1] = (uint32_t)(uintptr_t)sample.bytes;
    sample.words[63] = 0x31u;
    if (sample_layout((uint32_t)(uintptr_t)sample.bytes, marker) != 1 ||
        sample_layout((uint32_t)(uintptr_t)sample.bytes, marker) != 1) return 9;
    if (layout_sample_bytes() != 256 || layout_block_hash(0) == 0 ||
        layout_block_hash(3) == 0 || layout_block_hash(4) != 0) return 10;
    if (layout_normalized_block_hash(0) == 0 ||
        layout_normalized_block_hash(0) == layout_block_hash(0) ||
        layout_normalized_block_hash(4) != 0) return 16;
    if (layout_zero_low() != 0xFFFFFFFDu || layout_zero_high() != 0x7FFFFFFFu ||
        layout_pointer_low() != 0x00000002u || layout_pointer_high() != 0) return 11;
    if (observe_layout((uint32_t)(uintptr_t)sample.bytes, marker, 100) != 1 ||
        observe_layout((uint32_t)(uintptr_t)sample.bytes, marker, 100) != 1 ||
        layout_observation_count() != 1) return 12;
    sample.words[63] = 0x32u;
    if (observe_layout((uint32_t)(uintptr_t)sample.bytes, marker, 101) != 1 ||
        layout_observation_count() != 2) return 13;
    for (test_index = 102; test_index <= 131; ++test_index) {
        if (observe_layout((uint32_t)(uintptr_t)sample.bytes, marker, test_index) != 1) return 14;
    }
    if (observe_layout((uint32_t)(uintptr_t)sample.bytes, marker, 132) != 1 ||
        layout_observation_count() != 33) return 15;
    FreeLibrary(module);
    printf("OK: ABI=15 anchor portal and map visual fixes exported; host remains safely disabled\n");
    return 0;
}
