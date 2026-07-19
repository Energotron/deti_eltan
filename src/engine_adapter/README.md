# CE Second Map engine adapter

The first adapter build is deliberately non-invasive. It proves that the project
can produce a 32-bit DLL with the C ABI supported by RScript `ScriptLibs`.

Current ABI 4 capabilities:

- report ABI version 4 and explicit capability flags;
- round-trip a 32-bit value;
- retain the pointer returned by `GalaxyPtr()`;
- copy at most 256 verified readable bytes for one read-only layout sample per
  process, emitting hashes and classification masks but no raw values or addresses;
- explicitly report that native multi-galaxy switching is not implemented.

The bind, marker and 64-byte fingerprint gates passed in the supported game build.
The ABI 4 layout sampler passed the separate x86 host and three independent
in-game processes for `Rangers.exe 2.1.2500.0`. Save/load comparison remains the
next gate; write and native multi-galaxy capabilities stay disabled.

Build and test with `tools/build-engine-adapter.ps1`. The ABI contract and staged
in-game gates are documented in `docs/SECOND_MAP_ADAPTER_ABI.md`.
