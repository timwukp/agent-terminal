#!/usr/bin/env python3
"""Generate randomized fuzz corpus entries for fuzz_vt_feed.

Deterministic (seeded) so CI and local runs produce the same corpus.
Usage: python3 tools/gen_corpus.py [count] [outdir]
"""
import random
import sys
from pathlib import Path

TOKENS = [
    b'\x1b[', b'\x1b]', b'\x1bP', b'\x1b\\', b'\x07', b'\x1b[m', b'\x1b[2J',
    b'\x1b[?1049h', b'\x1b[?1049l', b'\x1b[10;10H', b'\x1b[38;5;196m',
    b'\xe4\xb8\xad', b'\xf0\x9f\x98\x80', b'\xc0\xaf', b'\xff', b'\x1b[999999@',
    b'hello', b'\r\n', b'\x1b[K', b'\x1b[L', b'\x1b[M', b'\x18', b'\x1b[;;;m',
    b'\x1b(0', b'qqq', b'\x1b(B', b'\x1bM', b'\x1b7', b'\x1b8', b'\x1b#8',
]

def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    outdir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('fuzz/corpus/vt')
    outdir.mkdir(parents=True, exist_ok=True)
    random.seed(42)
    for i in range(count):
        out = bytes(random.randrange(256) for _ in range(4))  # geometry header
        for _ in range(random.randrange(5, 60)):
            if random.random() < 0.7:
                out += random.choice(TOKENS)
            else:
                out += bytes(random.randrange(256)
                             for _ in range(random.randrange(1, 8)))
        (outdir / f'gen_{i:03d}').write_bytes(out)
    print(f'generated {count} entries in {outdir}')

if __name__ == '__main__':
    main()
