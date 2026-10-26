// Yaz0 decoder for tools/maxwell/yaz0.py (loaded with ctypes). Returns the number of bytes written. long long, not
// long: on Windows long is 32 bits.
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
__declspec(dllexport)
#endif
long long yaz0_decode(const uint8_t* in, size_t n, uint8_t* out, size_t size) {
    size_t src = 16, dst = 0;
    while (dst < size && src < n) {
        uint8_t flags = in[src++];
        for (int bit = 7; bit >= 0 && dst < size; bit--) {
            if (flags >> bit & 1) {
                if (src >= n) return (long long)dst;
                out[dst++] = in[src++];
            } else {
                if (src + 1 >= n) return (long long)dst;
                uint8_t b0 = in[src], b1 = in[src + 1];
                src += 2;
                size_t dist = ((size_t)(b0 & 0xF) << 8 | b1) + 1, len = b0 >> 4;
                if (len == 0) {
                    if (src >= n) return (long long)dst;
                    len = (size_t)in[src++] + 0x12;
                } else {
                    len += 2;
                }
                if (dist > dst) return -1;
                for (; len && dst < size; len--, dst++) out[dst] = out[dst - dist];
            }
        }
    }
    return (long long)dst;
}
