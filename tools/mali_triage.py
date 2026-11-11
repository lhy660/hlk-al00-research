import re, subprocess, sys, io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

R18 = "tools/mali-ref/ref-r18p0/driver/product/kernel/drivers/gpu/arm/midgard"
HW = "src/Code_Opensource/kernel/drivers/gpu/arm/bifrost-r18p0-01rel0"

diff = open("/tmp/mali_18to19.diff", encoding="utf-8", errors="replace").read()

cur_file = None
hunks = []  # (file, header, added_lines, removed_lines)

added = []
removed = []
header = None

for line in diff.splitlines():
    if line.startswith("--- "):
        cur_file = line.split()[1]
        continue
    if line.startswith("+++ "):
        cur_file = line.split()[1]
        continue
    if line.startswith("@@"):
        if header:
            hunks.append((cur_file, header, added, removed))
        header = line
        added = []
        removed = []
        continue
    if header is None:
        continue
    if line.startswith("+") and not line.startswith("+++"):
        added.append(line[1:])
    elif line.startswith("-") and not line.startswith("---"):
        removed.append(line[1:])

if header:
    hunks.append((cur_file, header, added, removed))

def norm(s):
    return re.sub(r"\s+", " ", s.strip())

missing = []
present = []
for f, h, a, r in hunks:
    fname = f.replace("tools/mali-ref/ref-r18p0/driver/product/kernel/drivers/gpu/arm/midgard/", "")
    import os
    hw_path = os.path.join(HW, fname)
    if not os.path.exists(hw_path):
        continue
    try:
        hw_src = open(hw_path, encoding="utf-8", errors="replace").read()
    except Exception:
        continue
    hw_norm = norm(hw_src)
    # significant added lines (>10 chars, not braces)
    sig = [norm(x) for x in a if len(x.strip()) > 10]
    if not sig:
        continue
    found = sum(1 for s in sig if s in hw_norm)
    if found == len(sig):
        present.append((fname, h, len(sig)))
    else:
        missing.append((fname, h, found, len(sig), sig[:3]))

print("=== hunks fully present in Huawei copy: %d ===" % len(present))
print("=== hunks with lines NOT found in Huawei copy: %d ===" % len(missing))
for fname, h, f2, t, sig in missing:
    print("\n%s :: %s  (matched %d/%d)" % (fname, h, f2, t))
    for s in sig:
        print("   + %s" % s[:150])
