#include <stdint.h>
#include <stdio.h>
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
    ce_dword_fn bind;
    ce_dword_fn echo;
    typedef uint32_t (__cdecl *ce_two_dword_fn)(uint32_t, uint32_t);
    ce_two_dword_fn run_smoke;
    ce_two_dword_fn probe;
    ce_noarg_fn fingerprint_hash;
    ce_noarg_fn fingerprint_bytes;
    uint8_t sample[64] = {0};
    const uint32_t marker = 0x1234ABCDu;

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
    supports_multi = (ce_noarg_fn)require_export(module, "CEAdapterSupportsNativeMultiGalaxy");

    if (abi() != 3 || capabilities() != 49 || supports_multi() != 0) return 3;
    if (echo(marker) != marker || bind(0) != 0 || bind(marker) != 1) return 4;
    if (get_bound() != marker) return 5;
    if (run_smoke(0, 1128616787u) != 0 || run_smoke(marker, 1128616787u) != 1) return 6;
    sample[0] = 0xCE;
    sample[63] = 0x31;
    if (probe((uint32_t)(uintptr_t)sample, 64) != 1) return 7;
    if (fingerprint_hash() == 0 || fingerprint_bytes() != 64) return 8;
    FreeLibrary(module);
    printf("OK: ABI=3 bind/marker/read-only fingerprint passed; native multi-galaxy remains disabled\n");
    return 0;
}
