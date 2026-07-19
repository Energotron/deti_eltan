# CE Second Map engine adapter

The first adapter build is deliberately non-invasive. It proves that the project
can produce a 32-bit DLL with the C ABI supported by RScript `ScriptLibs`.

Current capabilities:

- report ABI version 1;
- round-trip a 32-bit value;
- retain the pointer returned by `GalaxyPtr()` without dereferencing it;
- explicitly report that native multi-galaxy switching is not implemented.

It must not be enabled in the game until the `Main.dat` fragment and a harmless
`CEAdapterEchoDword` call pass a manual smoke test. Memory layout work starts only
from build-specific evidence for `Rangers.exe 2.1.2500.0`.

Build and test with `tools/build-engine-adapter.ps1`. The ABI contract and staged
in-game gates are documented in `docs/SECOND_MAP_ADAPTER_ABI.md`.
