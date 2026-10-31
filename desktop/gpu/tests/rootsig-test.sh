#!/bin/bash
# rootsig-test.sh <libxrl-d3d12.so> <DirectX-Headers include dir> - checks the shim's root signature serializer and
# container signing against blobs from Microsoft's own tools: vkd3d's d3d12 tests (pinned below) carry fxc-compiled
# root signatures (/T rootsig_1_0 and rootsig_1_1) next to the descriptions they were made from, and 66 fxc shaders.
# Every serialized description must equal its blob byte for byte (digest included), and every shader's digest must
# be what xrl_dxil_hash.c computes. Runs on any Linux box; no D3D12 needed.
set -euo pipefail
lib=$(realpath "$1"); inc=$(realpath "$2")
here=$(cd "$(dirname "$0")" && pwd)
vkd3d=23254b531ca5ff8e43f4aaae30b6b9f0d971850b
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
curl -sSfL -o "$w/d3d12.c" "https://gitlab.winehq.org/wine/vkd3d/-/raw/$vkd3d/tests/d3d12.c"
# the cases: test_root_signature_byte_code's static data, from its first blob to the end of its tests[] table
python3 -I - "$w/d3d12.c" "$w/cases.h" "$w/shaders.txt" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
f = s.index('static void test_root_signature_byte_code(void)')
a = s.index('    static const DWORD empty_rootsig[]', f)
t = s.index('    tests[] =', a)
b = s.index('\n    };\n', t) + len('\n    };\n')
body = re.sub(r'#if 0.*?#endif\n', '', s[a:b], flags=re.S)
open(sys.argv[2], 'w').write(body)
with open(sys.argv[3], 'w') as o:
    for m in re.finditer(r'\{\s*(0x43425844(?:\s*,\s*0x[0-9a-fA-F]+)+)\s*,?\s*\}', s):
        o.write(' '.join(m.group(1).replace(',', ' ').split()) + '\n')
PY
cat > "$w/t.c" <<'C'
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef HRESULT (*ser_fn)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, void **, size_t *, const char **);
#include "xrl_dxil_hash.h"
typedef void (*hash_fn)(const void *, uint32_t, uint8_t *);
static int check(ser_fn ser, const char *what, unsigned i, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d,
                 const void *code, size_t size)
{
    void *p; size_t n; const char *why = "";
    HRESULT hr = ser(d, &p, &n, &why);
    int ok = hr == S_OK && n == size && !memcmp(p, code, n);
    if (!ok) {
        printf("FAIL rootsig %u (%s): hr 0x%08x %s, %zu bytes, want %zu\n", i, what, (unsigned)hr, why, hr == S_OK ? n : 0, size);
        for (size_t k = 0; hr == S_OK && k < n / 4 && k < size / 4; k++)
            if (((const uint32_t *)p)[k] != ((const uint32_t *)code)[k])
                printf("   word %zu: 0x%08x, want 0x%08x\n", k, ((const uint32_t *)p)[k], ((const uint32_t *)code)[k]);
    }
    if (hr == S_OK) free(p);
    return ok;
}
int main(int argc, char **argv)
{
#include "cases.h"
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
    ser_fn ser = (ser_fn)dlsym(h, "xrl_d3d12_serialize_root_signature");
    hash_fn hash = xrl_dxil_hash_retail;
    int pass = 0, fail = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
        const struct test *t = &tests[i];
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v;
        if (t->code.pShaderBytecode) {
            memset(&v, 0, sizeof v); v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0; v.Desc_1_0 = *t->desc;
            check(ser, "1.0", i, &v, t->code.pShaderBytecode, t->code.BytecodeLength) ? pass++ : fail++;
        }
        memset(&v, 0, sizeof v); v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; v.Desc_1_1 = *t->desc1;
        check(ser, "1.1", i, &v, t->code1.pShaderBytecode, t->code1.BytecodeLength) ? pass++ : fail++;
    }
    printf("root signatures: %d pass, %d fail\n", pass, fail);
    /* shader digests: one container per line, hex words */
    FILE *f = fopen(argv[2], "r");
    static char line[1 << 22]; static uint32_t w[1 << 18];
    int sp = 0, sf = 0;
    while (f && fgets(line, sizeof line, f)) {
        int n = 0; char *q = line;
        while (*q && *q != '\n' && n < (1 << 18)) { w[n++] = (uint32_t)strtoul(q, &q, 16); while (*q == ' ') q++; }
        if (n < 8 || w[6] != (uint32_t)n * 4) continue;
        uint8_t d[16];
        hash((uint8_t *)w + 20, n * 4 - 20, d);
        if (memcmp(d, (uint8_t *)w + 4, 16)) sf++; else sp++;
    }
    printf("shader digests: %d pass, %d fail\n", sp, sf);
    return fail || sf || !pass || !sp;
}
C
cc -std=gnu11 -w -I"$inc" -I"$inc/wsl/stubs" -I"$w" -I"$here/../xrl-d3d12" "$w/t.c" "$here/../xrl-d3d12/xrl_dxil_hash.c" -o "$w/t" -ldl
"$w/t" "$lib" "$w/shaders.txt"
