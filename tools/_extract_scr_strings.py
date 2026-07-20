#!/usr/bin/env python3
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = path.read_bytes()
strings: list[tuple[str, str]] = []
i = 0
while i < len(data) - 1:
    if data[i + 1] == 0 and (32 <= data[i] < 127 or data[i] in (10, 13)):
        j = i
        out: list[str] = []
        while j < len(data) - 1 and data[j + 1] == 0 and (32 <= data[j] < 127 or data[j] in (10, 13)):
            out.append(chr(data[j]))
            j += 2
        s = "".join(out)
        if len(s) >= 4:
            strings.append(("ascii", s))
        i = j
        continue
    code = data[i] | (data[i + 1] << 8)
    if 0x0400 <= code <= 0x04FF or code in (10, 13, 32):
        j = i
        out = []
        while j < len(data) - 1:
            code = data[j] | (data[j + 1] << 8)
            if 0x0400 <= code <= 0x04FF or code in (10, 13, 32):
                out.append(chr(code))
                j += 2
            else:
                break
        s = "".join(out)
        if len(s) >= 4:
            strings.append(("ru", s))
        i = j
        continue
    i += 1
seen: set[str] = set()
for kind, s in strings:
    if s in seen:
        continue
    seen.add(s)
    print(f"[{kind}] {s}")
