/*
 * xrl_dxil_hash.c - the digest in a DXBC/DXIL container header, as the D3D12 runtime checks it.
 *
 * D3D12 refuses shader bytecode whose container digest does not match its contents (unless the process enabled
 * experimental shader models, which needs Windows developer mode). On Windows, Mesa gets the digest from dxil.dll's
 * validator; on Linux (WSL) it relies on experimental shader models. The console's runtime is a Windows one, so the
 * xbox-rl build of Mesa computes the digest itself after nir_to_dxil (patches/mesa) and the root-signature
 * serializer (xrl_d3d12.cpp) does the same for its RTS0 containers.
 *
 * The digest is MD5's compression function over the container from its Version field (byte 20) to its end, with
 * DXC's own padding (DxilHash.cpp, ComputeHashRetail): the bit count goes into the first word of the last block
 * and (size << 1) | 1 into its last word, instead of MD5's 64-bit length at the end.
 */
#include <stdint.h>
#include <string.h>
#include "xrl_dxil_hash.h"

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define STEP(f, a, b, c, d, x, s, ac) ((a) += f((b), (c), (d)) + (x) + (uint32_t)(ac), (a) = ROTL((a), (s)) + (b))

static void md5_block(uint32_t st[4], const uint32_t x[16])
{
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    STEP(F, a, b, c, d, x[0], 7, 0xd76aa478);  STEP(F, d, a, b, c, x[1], 12, 0xe8c7b756);
    STEP(F, c, d, a, b, x[2], 17, 0x242070db); STEP(F, b, c, d, a, x[3], 22, 0xc1bdceee);
    STEP(F, a, b, c, d, x[4], 7, 0xf57c0faf);  STEP(F, d, a, b, c, x[5], 12, 0x4787c62a);
    STEP(F, c, d, a, b, x[6], 17, 0xa8304613); STEP(F, b, c, d, a, x[7], 22, 0xfd469501);
    STEP(F, a, b, c, d, x[8], 7, 0x698098d8);  STEP(F, d, a, b, c, x[9], 12, 0x8b44f7af);
    STEP(F, c, d, a, b, x[10], 17, 0xffff5bb1); STEP(F, b, c, d, a, x[11], 22, 0x895cd7be);
    STEP(F, a, b, c, d, x[12], 7, 0x6b901122); STEP(F, d, a, b, c, x[13], 12, 0xfd987193);
    STEP(F, c, d, a, b, x[14], 17, 0xa679438e); STEP(F, b, c, d, a, x[15], 22, 0x49b40821);

    STEP(G, a, b, c, d, x[1], 5, 0xf61e2562);  STEP(G, d, a, b, c, x[6], 9, 0xc040b340);
    STEP(G, c, d, a, b, x[11], 14, 0x265e5a51); STEP(G, b, c, d, a, x[0], 20, 0xe9b6c7aa);
    STEP(G, a, b, c, d, x[5], 5, 0xd62f105d);  STEP(G, d, a, b, c, x[10], 9, 0x02441453);
    STEP(G, c, d, a, b, x[15], 14, 0xd8a1e681); STEP(G, b, c, d, a, x[4], 20, 0xe7d3fbc8);
    STEP(G, a, b, c, d, x[9], 5, 0x21e1cde6);  STEP(G, d, a, b, c, x[14], 9, 0xc33707d6);
    STEP(G, c, d, a, b, x[3], 14, 0xf4d50d87); STEP(G, b, c, d, a, x[8], 20, 0x455a14ed);
    STEP(G, a, b, c, d, x[13], 5, 0xa9e3e905); STEP(G, d, a, b, c, x[2], 9, 0xfcefa3f8);
    STEP(G, c, d, a, b, x[7], 14, 0x676f02d9); STEP(G, b, c, d, a, x[12], 20, 0x8d2a4c8a);

    STEP(H, a, b, c, d, x[5], 4, 0xfffa3942);  STEP(H, d, a, b, c, x[8], 11, 0x8771f681);
    STEP(H, c, d, a, b, x[11], 16, 0x6d9d6122); STEP(H, b, c, d, a, x[14], 23, 0xfde5380c);
    STEP(H, a, b, c, d, x[1], 4, 0xa4beea44);  STEP(H, d, a, b, c, x[4], 11, 0x4bdecfa9);
    STEP(H, c, d, a, b, x[7], 16, 0xf6bb4b60); STEP(H, b, c, d, a, x[10], 23, 0xbebfbc70);
    STEP(H, a, b, c, d, x[13], 4, 0x289b7ec6); STEP(H, d, a, b, c, x[0], 11, 0xeaa127fa);
    STEP(H, c, d, a, b, x[3], 16, 0xd4ef3085); STEP(H, b, c, d, a, x[6], 23, 0x04881d05);
    STEP(H, a, b, c, d, x[9], 4, 0xd9d4d039);  STEP(H, d, a, b, c, x[12], 11, 0xe6db99e5);
    STEP(H, c, d, a, b, x[15], 16, 0x1fa27cf8); STEP(H, b, c, d, a, x[2], 23, 0xc4ac5665);

    STEP(I, a, b, c, d, x[0], 6, 0xf4292244);  STEP(I, d, a, b, c, x[7], 10, 0x432aff97);
    STEP(I, c, d, a, b, x[14], 15, 0xab9423a7); STEP(I, b, c, d, a, x[5], 21, 0xfc93a039);
    STEP(I, a, b, c, d, x[12], 6, 0x655b59c3); STEP(I, d, a, b, c, x[3], 10, 0x8f0ccc92);
    STEP(I, c, d, a, b, x[10], 15, 0xffeff47d); STEP(I, b, c, d, a, x[1], 21, 0x85845dd1);
    STEP(I, a, b, c, d, x[8], 6, 0x6fa87e4f);  STEP(I, d, a, b, c, x[15], 10, 0xfe2ce6e0);
    STEP(I, c, d, a, b, x[6], 15, 0xa3014314); STEP(I, b, c, d, a, x[13], 21, 0x4e0811a1);
    STEP(I, a, b, c, d, x[4], 6, 0xf7537e82);  STEP(I, d, a, b, c, x[11], 10, 0xbd3af235);
    STEP(I, c, d, a, b, x[2], 15, 0x2ad7d2bb); STEP(I, b, c, d, a, x[9], 21, 0xeb86d391);
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void xrl_dxil_hash_retail(const void *data, uint32_t size, uint8_t out[16])
{
    static const uint8_t pad[64] = { 0x80 };
    const uint8_t *p = data;
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, x[16];
    uint32_t full = size / 64, left = size % 64, off = 0;
    for (uint32_t i = 0; i < full; i++, off += 64) {
        memcpy(x, p + off, 64);   /* little-endian host: the words as MD5 reads them */
        md5_block(st, x);
    }
    if (left < 56) {
        /* one last block: bit count, the remaining bytes, 0x80, zeros, (size << 1) | 1 */
        memset(x, 0, sizeof x);
        x[0] = size << 3;
        memcpy((uint8_t *)x + 4, p + off, left);
        memcpy((uint8_t *)x + 4 + left, pad, 56 - left);
        x[15] = 1 | (size << 1);
        md5_block(st, x);
    } else {
        /* the remaining bytes and the padding's start, then a block of bit count, padding, (size << 1) | 1 */
        uint32_t padn = 120 - left;
        memset(x, 0, sizeof x);
        memcpy(x, p + off, left);
        memcpy((uint8_t *)x + left, pad, padn - 56);
        md5_block(st, x);
        memset(x, 0, sizeof x);
        x[0] = size << 3;
        memcpy((uint8_t *)x + 4, pad + padn - 56, 56);
        x[15] = 1 | (size << 1);
        md5_block(st, x);
    }
    memcpy(out, st, 16);
}

void xrl_dxil_container_sign(void *container, uint32_t size)
{
    uint8_t *c = container;
    if (size < 32 || memcmp(c, "DXBC", 4)) return;
    xrl_dxil_hash_retail(c + 20, size - 20, c + 4);
}
