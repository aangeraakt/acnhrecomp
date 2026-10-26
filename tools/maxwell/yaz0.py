"""Yaz0 (the console's LZ77 variant, ".szs"): the compression Animal Crossing: New Horizons uses next to zstd.

Header: "Yaz0", u32 big-endian decompressed size, 8 reserved bytes. Then groups of a flag byte (MSB first) and
eight items: a set bit copies one literal byte, a clear bit is a back-reference: two bytes `NR RR` (distance
`0xRRR + 1`, length `N + 2`), or when N == 0 a third byte gives length `byte + 0x12`.

A small C decoder (tools/maxwell/yaz0_fast.c, built once into private/) is used when a C compiler is around;
otherwise the pure Python loop below (slow: ~3 MB/s).
"""
import ctypes
import os
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
    out = os.path.join(_ROOT, "private", "build", "libyaz0.so")
    try:
        if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
            os.makedirs(os.path.dirname(out), exist_ok=True)
            cc = os.environ.get("CC", "cc")
            subprocess.run([cc, "-O2", "-shared", "-fPIC", "-o", out, src], check=True, capture_output=True)
        lib = ctypes.CDLL(out)
        lib.yaz0_decode.restype = ctypes.c_long
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
