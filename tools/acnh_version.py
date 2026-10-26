#!/usr/bin/env python3
"""Which version of ACNH a game folder holds, and the profile it is built with (games/acnh/versions.txt).

usage: acnh_version.py <data folder> [--get <key>]

Reads main's build id from <folder>/exefs/main, and the update's title version from the game.ini the game folder may
carry next to its exefs/ (written by the tool that prepared it; nothing else is read). The result goes to
<folder>/version.ini:
    build_id = FCD2BB238ABE99E925B4E452DF1F41F1
    version = 3.0.2                 (from versions.txt, else from the title version, else "unknown")
    title_version = 2162688         (-1: not known)
    hooks = games/acnh/hooks.txt    (- for a build versions.txt does not list: no hooks)
--get <key> prints one value of the result (tools/setup_acnh.py reads hooks and version this way).
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "games", "acnh", "versions.txt")
# Title versions whose number is known (release << 16). The base game is always 0.
NAMES = {0: "1.0.0", 29 << 16: "2.0.7", 33 << 16: "3.0.2"}


def build_id(path):
    with open(path, "rb") as f:
        head = f.read(0x50)
    if head[:4] != b"NSO0":
        sys.exit(f"{path}: not an NSO")
    return head[0x40:0x50].hex().upper()


def known_builds():
    out = {}
    for line in open(TABLE, encoding="utf-8"):
        f = line.split("#")[0].split()
        if len(f) == 3:
            out[f[0].upper()] = (f[1], f[2])
    return out


def read_ini(path):
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            k, _, v = line.partition("=")
            if _:
                out[k.strip()] = v.strip()
    return out


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        sys.exit(__doc__)
    folder, get = args[0], None
    i = 1
    while i < len(args):
        if args[i] == "--get" and i + 1 < len(args):
            get, i = args[i + 1], i + 2
        else:
            sys.exit(f"unknown argument {args[i]}")

    ini_path = os.path.join(folder, "version.ini")
    exefs = os.path.join(folder, "exefs")
    bid = build_id(os.path.join(exefs, "main"))
    # The game folder's own description (next to its exefs/, which the data folder links to).
    game = read_ini(os.path.join(os.path.dirname(os.path.realpath(exefs)), "game.ini"))
    tv = int(game.get("title_version", -1)) if game.get("build_id", bid).upper() == bid else -1

    known = known_builds().get(bid)
    if known:
        version, hooks = known
    else:
        version = NAMES.get(tv, f"release {tv >> 16}" if tv > 0 else "unknown")
        hooks = "-"
    res = {"build_id": bid, "version": version, "title_version": str(tv), "hooks": hooks}
    if get:
        print(res[get])
        return
    with open(ini_path + ".tmp", "w", encoding="utf-8") as f:
        f.write("# Written by tools/acnh_version.py: the version this folder holds and its build profile.\n")
        f.writelines(f"{k} = {v}\n" for k, v in res.items())
    os.replace(ini_path + ".tmp", ini_path)
    profile = f"hooks from {hooks}" if known else "not in games/acnh/versions.txt: built without hooks (docs/VERSIONS.md)"
    print(f"version {version} (main build id {bid}): {profile}")


if __name__ == "__main__":
    main()
