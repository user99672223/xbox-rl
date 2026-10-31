/* xrl_dxil_hash.h - the DXBC/DXIL container digest the D3D12 runtime checks (xrl_dxil_hash.c). */
#ifndef XRL_DXIL_HASH_H
#define XRL_DXIL_HASH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* DXC's retail hash (modified MD5) of `size` bytes */
void xrl_dxil_hash_retail(const void *data, uint32_t size, uint8_t out[16]);
/* writes the digest of a whole DXBC container (bytes 20..size) into its header (bytes 4..19) */
void xrl_dxil_container_sign(void *container, uint32_t size);
#ifdef __cplusplus
}
#endif
#endif
