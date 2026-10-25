#!/usr/bin/env python3
"""Names the functions of a CPU profile written by the runtime (NX_CPU_PROFILE, runtime/src/cpu_profile.cpp).

usage: tools/cpu_profile.py <cpu_profile_<thread>.txt> [binary] [--top N]
The file holds "slide <hex> samples <n> interval_us <us>" then "<pc hex> <count>" lines. The binary's symbols
(local ones too, which dladdr cannot see) come from `nm -n`; a recompiled function's C++ name (__Z13main_03B0EEDC...)
is shown as main_03B0EEDC, the guest function it is.
"""
import bisect
import re
import subprocess
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    prof = args[0]
    binary = args[1] if len(args) > 1 else "private/acnh/build/acnh"
    with open(prof) as f:
        head = f.readline().split()
        slide = int(head[1], 16)
        samples = int(head[3])
        counts = []
        for line in f:
            parts = line.split(maxsplit=2)
            if len(parts) >= 2:
                counts.append((int(parts[0], 16), int(parts[1]), parts[2].strip() if len(parts) > 2 else ""))
    out = subprocess.run(["nm", "-n", binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) == 3 and parts[1] in "tT":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    per = {}
    outside = 0
    for pc, n, ext in counts:
        if ext:  # named by the runtime (a system library)
            name = ext.split(":")[0] + ":" + re.sub(r"^_+", "", ext.split(":", 1)[1])[:80]
            per[name] = per.get(name, 0) + n
            continue
        a = pc - slide
        k = bisect.bisect_right(addrs, a) - 1
        if k < 0 or a - addrs[k] > 0x100000:  # not in this binary (system libraries, MoltenVK, the driver)
            outside += n
            continue
        name = names[k]
        m = re.search(r"((?:main|sdk|subsdk\d)_[0-9A-F]{8})", name)
        if m:
            name = m.group(1) + " (game)"
        per[name] = per.get(name, 0) + n
    total = sum(per.values()) + outside
    waiting = sum(n for name, n in per.items()
                  if re.search(r"cvwait|semaphore_.*wait|semwait|workq_kernreturn|mach_msg|ulock_wait|kevent|select|nanosleep|thread_switch", name))
    print(f"  busy (not waiting): {100.0 * (total - waiting) / max(total, 1):.1f}%")
    libs = {}
    for name, n in per.items():
        if ":" in name and not name.endswith("(game)"):
            libs[name.split(":")[0]] = libs.get(name.split(":")[0], 0) + n
    if libs:
        print("  by library: " + ", ".join(f"{k} {100.0 * v / max(total, 1):.1f}%" for k, v in sorted(libs.items(), key=lambda kv: -kv[1])[:6]))
    print(f"{prof}: {samples} samples")
    game = sum(n for name, n in per.items() if name.endswith("(game)"))
    print(f"  in recompiled game code: {100.0 * game / max(total, 1):.1f}%, outside the binary (system, Metal): "
          f"{100.0 * outside / max(total, 1):.1f}%")
    for name, n in sorted(per.items(), key=lambda kv: -kv[1])[:top]:
        print(f"  {100.0 * n / max(total, 1):5.1f}%  {n:7d}  {name[:110]}")


if __name__ == "__main__":
    main()
