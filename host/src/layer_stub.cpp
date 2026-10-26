// layer_stub.cpp - stand-in for Workstream C's xrl_layer_boot() until layer.lib
// exists. A strong definition from layer.lib (/p:XrlLayerLib=...) wins at link
// time via the /alternatename binding below; this file then becomes dead weight
// and can be dropped from the project.
//
// What the stub proves: the host built a valid boot context, the op tables are
// callable, and logging works from "inside the layer" (it logs through ctx->log,
// exactly the way the real layer must).
#include "pch.h"
#include "xrl_host.h"

extern "C" int  xrl_layer_boot_stub(const xrl_boot_context* ctx);
extern "C" void xrl_layer_request_exit_stub(int code);
#pragma comment(linker, "/alternatename:xrl_layer_boot=xrl_layer_boot_stub")
#pragma comment(linker, "/alternatename:xrl_layer_request_exit=xrl_layer_request_exit_stub")

extern "C" int xrl_layer_boot_stub(const xrl_boot_context* ctx) {
    if (!ctx || ctx->struct_size < sizeof(xrl_boot_context) || ctx->abi_version != XRL_ABI_VERSION) {
        XLOGE("layer-stub", "bad boot context (size/abi mismatch)");
        return XRL_ERR_GENERIC;
    }
    ctx->log(1, "layer-stub", "xrl_layer_boot STUB reached - no layer linked (build with /p:XrlLayerLib=<layer.lib> to replace)");
    ctx->log(1, "layer-stub", "boot context validated; returning -1 (XRL_ERR_GENERIC)");
    return XRL_ERR_GENERIC;
}

extern "C" void xrl_layer_request_exit_stub(int code) {
    XLOGI("layer-stub", "xrl_layer_request_exit(%d) (no layer; ignored)", code);
}
