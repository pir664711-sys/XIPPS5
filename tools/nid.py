#!/usr/bin/env python3
"""NID helper for ps5emu.

  python tools/nid.py hash malloc sceKernelUsleep      # name -> NID
  python tools/nid.py match ps5emu.imports.txt names.txt   # which imports does a name list explain?

names.txt: one function name per line. Copy it to nids.txt next to ps5emu.exe and the
log/imports table will show those names instead of raw NIDs.
"""
import base64, hashlib, sys

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(name: str) -> str:
    h = hashlib.sha1(name.encode() + SALT).digest()[:8][::-1]
    return base64.b64encode(h).decode().rstrip("=").replace("/", "-")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "hash":
        for n in sys.argv[2:]:
            print(nid(n), n)
    elif len(sys.argv) == 4 and sys.argv[1] == "match":
        want = {}
        for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4 and p[0] and not p[0].startswith("#"):
                want[p[0]] = p
        names = [l.strip() for l in open(sys.argv[3]) if l.strip() and not l.startswith("#")]
        hit = {nid(n): n for n in names}
        n_hit = 0
        for k, n in hit.items():
            if k in want:
                n_hit += 1
                print(f"{k}\t{n}\t{want[k][3]}")
        print(f"# {n_hit} of {len(want)} imports explained", file=sys.stderr)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
