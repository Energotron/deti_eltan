# CE Second Map engine adapter

The first adapter build is deliberately non-invasive. It proves that the project
can produce a 32-bit DLL with the C ABI supported by RScript `ScriptLibs`.

Current ABI 6 capabilities:

- report ABI version 6 and explicit capability flags;
- round-trip a 32-bit value;
- retain the pointer returned by `GalaxyPtr()`;
- copy at most 256 verified readable bytes for one read-only layout sample per
  process, emitting hashes and classification masks but no raw values or addresses;
- keep one bounded `latest.json` observation keyed by confirmed `CurTurn()`;
- hash a local copy with readable pointer-class dwords replaced by zero;
- explicitly report that native multi-galaxy switching is not implemented.

The bind, marker and 64-byte fingerprint gates passed in the supported game build.
ABI 5 passed the in-process `CurTurn 300 -> 301` game test. ABI 6 passes the x86
host and adds pointer-normalized hashes for the next save/load comparison.
Same-turn load observation is blocked because restored RScript does not call the
DLL until another game day. Write and native multi-galaxy capabilities stay disabled.

Build and test with `tools/build-engine-adapter.ps1`. The ABI contract and staged
in-game gates are documented in `docs/SECOND_MAP_ADAPTER_ABI.md`.
