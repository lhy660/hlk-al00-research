#!/usr/bin/env python3
"""Extract kallsyms from a raw physical kernel-image dump (kimage.bin).

vmlinux-to-elf's finder refuses this image because four byte runs look like
kallsyms_token_table.  We select the right candidate by masking the others
for the duration of the search and validating the resulting symbol table.

Usage:
  python tools/kallsyms_raw.py kimage.bin [symdump.txt]

Output lines are "address type name", the same format kallsyms-finder writes.
"""
import sys
from struct import pack

from vmlinux_to_elf.core.kallsyms import KallsymsFinder

DIGITS = b"".join(b"%c\0" % i for i in range(ord("0"), ord("9") + 1))
AVOID = (b":\0", b"\0\0", b"\0\1", b"\0\2", b"ASCII\0")


def candidates(img):
    out = []
    pos = -1
    while True:
        pos = img.find(DIGITS, pos + 1)
        if pos < 0:
            break
        tail = pos + len(DIGITS)
        if any(img[tail:tail + len(a)] == a for a in AVOID):
            continue
        out.append(pos)
    return out


def table_start(img, digit0):
    """Replicate KallsymsFinder.find_kallsyms_token_table's backward walk."""
    pos = digit0 - 1
    if img[pos] != 0:
        raise ValueError("no NUL before token '0'")
    for _ in range(ord("0")):
        for _ in range(50):
            pos -= 1
            if pos < 0:
                raise ValueError("walked off the image")
            if img[pos] == 0 or img[pos] > ord("z"):
                break
        else:
            raise ValueError("token chunk longer than 50")
    pos += 1
    pos += -pos % 4
    return pos


def token_offsets(img, start):
    """Replicate find_kallsyms_token_index's forward parse; null on error."""
    offs = []
    pos = start - 1
    for _ in range(256):
        pos += 1
        offs.append(pos - start)
        for _ in range(50):
            pos += 1
            if pos >= len(img):
                return None
            if img[pos] == 0:
                break
        else:
            return None
    return offs, pos


def validate(img, cand):
    """True only if the 256-entry u16 token index follows this table."""
    try:
        start = table_start(img, cand)
    except ValueError:
        return None
    parsed = token_offsets(img, start)
    if parsed is None:
        return None
    offs, pos = parsed
    needle = pack("<256H", *offs)
    if img[pos:pos + 512 + 256].find(needle) < 0:
        return None
    return start


class PickFinder(KallsymsFinder):
    pick = 0

    def find_kallsyms_token_table(self):
        img = self.kernel_img
        cands = candidates(img)
        if self.pick >= len(cands):
            raise ValueError("only %d token-table candidates" % len(cands))
        sel = cands[self.pick]
        mutated = bytearray(img)
        for p in cands:
            if p != sel:
                mutated[p:p + 2] = b"\x01\x01"
        self.kernel_img = bytes(mutated)
        try:
            super().find_kallsyms_token_table()
        finally:
            self.kernel_img = img
        self.used_candidate_offset = sel

    # The dump is a live, already-relocated image; never re-apply RELA.
    def find_elf64_rela(self):
        self.elf64_rela = None

    def apply_elf64_rela(self):
        pass


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    img_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "kimage.kallsyms"
    with open(img_path, "rb") as f:
        img = f.read()

    cands = candidates(img)
    print("token-table candidates: %r" % cands, file=sys.stderr)
    valid = [i for i, c in enumerate(cands) if validate(img, c) is not None]
    print("candidates with a matching token index: %r" % valid, file=sys.stderr)

    finder = None
    for pick in (valid or list(range(len(cands)))):
        try:
            picked = type("PickedFinder", (PickFinder,), {"pick": pick})
            f = picked(img, bit_size=64)
        except Exception as exc:  # noqa: BLE001 - try the next candidate
            print("candidate %d failed: %s" % (pick, exc), file=sys.stderr)
            continue
        names = f.name_to_symbol
        missing = [n for n in ("_text", "commit_creds", "modprobe_path",
                               "selinux_enforcing") if n not in names]
        ok = len(names) > 20000 and "_text" in names
        print("candidate %d @0x%x: %d symbols, missing=%r"
              % (pick, cands[pick], len(names), missing), file=sys.stderr)
        for probe in ("_text", "commit_creds", "modprobe_path",
                      "selinux_enforcing", "init_cred"):
            if probe in names:
                print("  %s = 0x%x" % (probe, names[probe].virtual_address),
                      file=sys.stderr)
            for extra in [n for n in names if probe in n][:4]:
                print("  like %-40s 0x%x" % (extra,
                                             names[extra].virtual_address),
                      file=sys.stderr)
        if ok:
            finder = f
            break

    if finder is None:
        sys.exit("no candidate produced a sane symbol table")

    syms = sorted(finder.symbols, key=lambda s: s.virtual_address)
    with open(out_path, "w") as f:
        for s in syms:
            f.write("%016x %s %s\n"
                    % (s.virtual_address, s.symbol_type.value, s.name))
    print("wrote %d symbols to %s" % (len(syms), out_path), file=sys.stderr)


if __name__ == "__main__":
    main()
