"""Yaz0 (the console's LZ77 variant, ".szs"): the compression Animal Crossing: New Horizons uses next to zstd.

Header: "Yaz0", u32 big-endian decompressed size, 8 reserved bytes. Then groups of a flag byte (MSB first) and
eight items: a set bit copies one literal byte, a clear bit is a back-reference: two bytes `NR RR` (distance
`0xRRR + 1`, length `N + 2`), or when N == 0 a third byte gives length `byte + 0x12`.

A small C decoder (tools/maxwell/yaz0_fast.c, built once into private/) is used when a C compiler is around (on
Windows clang, which tools/setup_acnh.py downloads); otherwise the pure Python loop below (slow: ~3 MB/s).
"""
import ctypes
import glob
import os
import shutil
import struct
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
_LIB = None


def _fast():
    global _LIB
    if _LIB is not None:
        return _LIB or None
    src = os.path.join(_HERE, "yaz0_fast.c")
    win = os.name == "nt"
    out = os.path.join(_ROOT, "private", "build", "yaz0.dll" if win else "libyaz0.so")
    try:
        if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if win:
                # A DLL with no C runtime (the decoder calls nothing), so no Visual Studio environment is needed.
                # clang from PATH, else the LLVM tools/setup_acnh.py put in private/third_party.
                llvm = sorted(glob.glob(os.path.join(_ROOT, "private", "third_party", "llvm-*", "bin", "clang.exe")))
                cmd = [shutil.which("clang") or (llvm[-1] if llvm else "clang"), "-O2", "-ffreestanding", "-shared", "-nostdlib",
                       "-fuse-ld=lld", "-Wl,/noentry", "-o", out, src]
            else:
                cmd = [os.environ.get("CC", "cc"), "-O2", "-shared", "-fPIC", "-o", out, src]
            subprocess.run(cmd, check=True, capture_output=True)
        lib = ctypes.CDLL(out)
        lib.yaz0_decode.restype = ctypes.c_longlong
        lib.yaz0_decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        _LIB = lib
    except Exception:
        _LIB = False
    return _LIB or None


def is_yaz0(data):
    return bytes(data[:4]) == b"Yaz0"


def decompress(data):
    data = bytes(data)
    if data[:4] != b"Yaz0":
        raise ValueError("not Yaz0")
    size = struct.unpack(">I", data[4:8])[0]
    lib = _fast()
    if lib:
        out = ctypes.create_string_buffer(size)
        n = lib.yaz0_decode(data, len(data), out, size)
        if n != size:
            raise ValueError(f"Yaz0: decoded {n} of {size} bytes")
        return bytearray(out.raw)
    out = bytearray(size)
    src, dst, n = 16, 0, len(data)
    while dst < size and src < n:
        flags = data[src]
        src += 1
        for bit in range(7, -1, -1):
            if dst >= size:
                break
            if flags >> bit & 1:
                out[dst] = data[src]
                dst += 1
                src += 1
            else:
                b0, b1 = data[src], data[src + 1]
                src += 2
                dist = ((b0 & 0xF) << 8 | b1) + 1
                ln = b0 >> 4
                if ln == 0:
                    ln = data[src] + 0x12
                    src += 1
                else:
                    ln += 2
                for _ in range(ln):
                    out[dst] = out[dst - dist]
                    dst += 1
    return out
