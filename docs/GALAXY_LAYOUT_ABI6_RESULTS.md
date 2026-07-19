# Galaxy layout ABI 6 — in-game results

## Result

The pointer-normalized save/load gate passed on 19 July 2026 with the isolated
`ChildrenOfEltanSmoke` module and Steam build 20648864.

The control cycle was:

1. load turn 301 in PID 18144;
2. capture `before` with the read-only attach scanner;
3. advance exactly one day and capture adapter output for turn 302;
4. save into a new slot and close the game;
5. restart, load the same turn 302 slot, and capture `reload` in PID 8452.

## Observations

- All four raw 64-byte block hashes changed across the process restart.
- Pointer-normalized blocks 0 and 3 were identical between `after` and
  same-turn `reload`.
- The zero mask remained `0D58404000000400`.
- The readable-pointer mask remained `00070001F000F801`.
- Discovery returned exactly one strict candidate in both processes.
- The adapter and scanner performed read-only sampling; no process memory was
  written and no addresses or raw values were archived.

The analyzer reported:

```text
Pointer-normalized blocks identical across save/load: [0, 3]
Zero mask preserved across save/load: True
Pointer-class mask preserved across save/load: True
PASS: turn advance and cross-process save/load samples are comparable
```

## Meaning

The normalized fingerprint is stable enough for the next read-only layout
analysis gate. This result does not prove field types, permit writes, create a
second Galaxy, extend the save format, or enable native switching. Those remain
separate capability gates.
