#!/usr/bin/env python3
"""Regenerate the single MANIFEST.md5 for the release package.

Run from the package root:   python src/gen_manifest.py
Verify with:                 md5sum -c MANIFEST.md5

Covers EVERY file in bin/, src/, docs/, history/ and the root (including
Markdown docs), except MANIFEST.md5 itself and the transient build/ dir.
After changing any file, re-run this script before shipping.
"""
import hashlib
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
os.chdir(ROOT)


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    rows = []
    for dp, dn, fn in os.walk('.'):
        dn[:] = [d for d in dn if d not in ('build', '__pycache__')]
        for name in fn:
            rel = os.path.relpath(os.path.join(dp, name), '.').replace('\\', '/')
            if rel == 'MANIFEST.md5':
                continue
            rows.append((md5(rel), rel))
    with open('MANIFEST.md5', 'w', newline='\n', encoding='utf-8') as out:
        for digest, rel in sorted(rows):
            out.write('%s  %s\n' % (digest, rel))
    print('wrote MANIFEST.md5: %d entries' % len(rows))


if __name__ == '__main__':
    main()
