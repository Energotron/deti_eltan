# Galaxy pointer topology — in-game results

## Purpose

Extend the proven 256-byte Galaxy fingerprint by following only readable root
pointers and hashing at most 64 bytes of each target. The scanner remains
read-only and publishes no addresses or raw values.

## Control cycle

- `before`: turn 302, PID 8452;
- `after`: turn 303, PID 8452;
- save into a new slot, close and restart;
- `reload`: turn 303, PID 5372.

All three captures used zero mask `0D58404000000400` and pointer mask
`00070001F000F801`.

## Results

The root contains 14 readable pointer-class dwords at indexes:

```text
0, 11, 12, 13, 14, 15, 28, 29, 30, 31, 32, 48, 49, 50
```

Normalized target fingerprints changed after one turn at:

```text
28, 30, 31, 32, 50
```

Targets whose normalized hash and classification masks matched after same-turn
save/load were:

```text
0, 11, 12, 13, 14, 15, 28, 29, 30, 31, 32, 50
```

The analyzer reported:

```text
PASS: read-only Galaxy pointer topology is comparable across save/load
```

## Ambiguity found and contained

Immediately after advancing to turn 303, the mask-only scan found two root
candidates. Both pointed to the same 14 target fingerprints, consistent with a
live and retained wrapper sharing underlying state. The adapter's callback raw
hashes selected exactly one current `GalaxyPtr()` through `--root-hashes`.
After restart and same-turn load, the strict mask again selected one candidate.

No field type is inferred from these indexes. In particular, the result does
not identify stars, sectors, fleets, ownership, constructors, or save records.
Writes, allocation, native switching, and save extension remain disabled.
