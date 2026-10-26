/*
 * xrl_abi.h — xbox-rl shared ABI contract.
 *
 * This header is the FROZEN seam between the four workstreams. It is owned by
 * the integrator (top level), not by any single subagent. Changing anything in
 * here is a cross-workstream event: it must be versioned via XRL_ABI_VERSION
 * and recorded in docs/decisions.md. Changes are APPEND-ONLY: existing struct
 * members never move, new ones go at the end, and every table carries
 * struct_size so a reader can tell which members the writer knew about.
 *
 * Three seams live here:
 *
 *   1. The syscall gateway (wine/ -> layer/). musl's __syscall and Wine's few
 *      raw syscall sites are patched to call xrl_syscall() instead of emitting
 *      a `syscall` instruction. The binary therefore contains no syscall
 *      instructions and runs native on the locked-down Windows host. The
 *      non-1:1 gateway contracts (clone, exit, rt_sigreturn, __xrl_gateway)
 *      are specified in docs/syscall-abi.md.
 *
 *   2. The boot/loader entry (host/ -> layer/). The UWP host hands the layer a
 *      fully-populated boot context and asks it to load the ELF image and run.
 *
 *   3. The host op-table (wine/ -> host/ + gfx/). Wine's builtin d3d12/dxgi/
 *      d3d11/xaudio2/xinput DLLs forward to native Xbox D3D12/XAudio2/
 *      Windows.Gaming.Input through these vtables. The Wine side never touches
 *      a WinRT or D3D *header*; it gets opaque handles and plain C tables.
 *
 * CALLING CONVENTIONS (v2 — the question all four workstreams raised):
 *   - Every function pointer in xrl_gfx_ops / xrl_audio_ops / xrl_input_ops and
 *     xrl_log_fn is a NATIVE host function: Microsoft x64 ABI, expects the host
 *     TEB in %gs and a host-valid stack. The host fills the tables with these.
 *   - Guest (SysV) code never calls them directly. It obtains guest-callable
 *     copies through the pseudo-syscalls below; each slot in a copy is a
 *     layer-generated SysV->MS-x64 stub that moves to the host stack and makes
 *     the native call. Constraint the stubs rely on: integer/pointer arguments
 *     only, at most 8 per function. Keep it that way when appending ops.
 *   - xrl_log_fn must be thread-safe: it is called from every guest thread.
 *   - HANDLES: an xrl_handle that names a COM object (device, queue, swapchain,
 *     resource, XAudio2 engine/voice) IS the raw native interface pointer
 *     (ID3D12Device*, IDXGISwapChain1*, ...). Wine hands it to the game, which
 *     calls COM methods on it (MS-x64, from PE code). Making those calls safe
 *     from guest threads is the layer's job (GS/TEB state, see
 *     docs/decisions.md "GS/TEB"). Wine never wraps them except where noted
 *     (the D3D11 swapchain buffer path).
 */
#ifndef XRL_ABI_H
#define XRL_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XRL_ABI_VERSION 2u

/* ------------------------------------------------------------------ *
 * Seam 1: the syscall gateway  (wine/ -> layer/)
 * ------------------------------------------------------------------ */

/*
 * The one entry every patched syscall site funnels through. `nr` is a Linux
 * x86-64 syscall number (see docs/syscall-abi.md for the subset the layer
 * implements) or one of the XRL pseudo-syscalls below. Arguments are 64-bit;
 * a6 is the 7th C argument (on the stack in SysV) and is always read — every
 * patched musl stub zeroes it when unused.
 *
 * Return value follows the Linux kernel convention, NOT libc: success is a
 * non-negative value (or a pointer); failure is the negated errno in
 * [-4095, -1]. musl's patched __syscall returns it verbatim.
 *
 * int64_t, not long: MSVC's long is 32-bit (v1 bug, fixed in v2). On the SysV
 * side int64_t is long, so the guest-visible signature is unchanged.
 *
 * May block (futex, read, poll). Reentrant and thread-safe.
 */
int64_t xrl_syscall(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                    int64_t a4, int64_t a5, int64_t a6);

/*
 * Pseudo-syscalls (v2), outside the Linux number space, answered inside
 * xrl_syscall. Each returns a pointer to a guest-callable (SysV-stubbed) copy
 * of the host table, or -ENOSYS if the host provides none. struct_size in the
 * returned table is the host's. Used by wine's dlls/xrlhost.
 */
#define XRL_SYS_GET_GFX_OPS    0x10001
#define XRL_SYS_GET_AUDIO_OPS  0x10002
#define XRL_SYS_GET_INPUT_OPS  0x10003
#define XRL_SYS_GET_LOG_FN     0x10004

/*
 * Guest thread pointer slot (v2, added after the first console run).
 * The Xbox hides FSGSBASE (CPUID.7.EBX[0] = 0), so the guest's thread pointer
 * (what Linux keeps in the FS base) cannot live in FS. It lives in the host
 * TEB's TlsSlots[XRL_TP_TLS_INDEX], i.e. at gs:[XRL_TP_GS_OFFSET], which
 * Windows keeps per thread. Guest ELFs are compiled with
 * -mno-tls-direct-seg-refs (and without a TLS stack-protector guard), so every
 * %fs use is `mov %fs:0,%reg`; tools/xrl-tpfix.py rewrites each one to
 * `mov %gs:XRL_TP_GS_OFFSET,%reg` (same length) and fails on anything else.
 * The layer claims the slot at boot and keeps it equal to the thread's
 * arch_prctl(ARCH_SET_FS) / CLONE_SETTLS value. Wine's PE TlsAlloc must never
 * hand out this index.
 */
#define XRL_TP_TLS_INDEX  63
#define XRL_TP_GS_OFFSET  0x1678   /* TEB.TlsSlots (0x1480) + 63 * 8 */

/*
 * Shared-TEB swap set (docs/gs-teb.md). Guest code (Wine's PE side, the game) and
 * host code (Windows ntdll, the CRT, D3D, WinRT, the layer) share one TEB per OS
 * thread because GS cannot be rebased. These fields both sides use with
 * different meanings: the layer keeps a guest and a host copy and swaps them on
 * every guest<->host transition, and Wine, when it adopts the host TEB, moves
 * its freshly initialised values for exactly these fields into the live TEB.
 * X(offset, qwords), x86-64 TEB offsets. TlsSlots[XRL_TP_TLS_INDEX] is excluded
 * on purpose: the guest thread pointer must stay live in both views.
 */
#define XRL_TEB_SWAP_RANGES(X) \
    X(0x008, 2)   /* NtTib.StackBase, StackLimit                          */ \
    X(0x038, 1)   /* EnvironmentPointer                                   */ \
    X(0x040, 2)   /* ClientId                                             */ \
    X(0x058, 2)   /* ThreadLocalStoragePointer, ProcessEnvironmentBlock   */ \
    X(0x068, 1)   /* LastErrorValue, CountOfOwnedCriticalSections         */ \
    X(0x108, 1)   /* CurrentLocale, FpSoftwareStatusRegister              */ \
    X(0x290, 5)   /* ActivationContextStack (embedded; 0x2c8 points here) */ \
    X(0x2c8, 1)   /* ActivationContextStackPointer                        */ \
    X(0x7d8, 2)   /* RealClientId                                         */ \
    X(0x1250, 1)  /* LastStatusValue                                      */ \
    X(0x1478, 64) /* DeallocationStack, TlsSlots[0..62]                   */ \
    X(0x1758, 1)  /* ReservedForOle (COM / WinRT apartment state)         */ \
    X(0x1778, 2)  /* ThreadPoolData, TlsExpansionSlots                    */ \
    X(0x17a0, 1)  /* NlsCache                                             */ \
    X(0x17c8, 5)  /* FlsData, Preferred/User/MergedPrefLanguages, flags   */
#define XRL__TEB_SUM(off, n) + (n)
#define XRL_TEB_SWAP_QWORDS (0 XRL_TEB_SWAP_RANGES(XRL__TEB_SUM))   /* 91 */

/*
 * Linux errno values the layer returns (negated) across xrl_syscall.
 */
enum {
    XRL_EPERM   = 1,   XRL_ENOENT  = 2,   XRL_ESRCH   = 3,   XRL_EINTR   = 4,
    XRL_EIO     = 5,   XRL_EBADF   = 9,   XRL_EAGAIN  = 11,  XRL_ENOMEM  = 12,
    XRL_EACCES  = 13,  XRL_EFAULT  = 14,  XRL_EBUSY   = 16,  XRL_EEXIST  = 17,
    XRL_ENODEV  = 19,  XRL_ENOTDIR = 20,  XRL_EISDIR  = 21,  XRL_EINVAL  = 22,
    XRL_ENFILE  = 23,  XRL_EMFILE  = 24,  XRL_ENOSPC  = 28,  XRL_ESPIPE  = 29,
    XRL_EPIPE   = 32,  XRL_ERANGE  = 34,  XRL_ENOSYS  = 38,  XRL_ENOTSUP = 95,
    XRL_ETIMEDOUT = 110,
};

/* ------------------------------------------------------------------ *
 * Seam 3: the host op-table  (wine/ -> host/ + gfx/)
 * ------------------------------------------------------------------ */

typedef uint64_t xrl_handle;          /* opaque; 0 means null/invalid        */
#define XRL_INVALID_HANDLE ((xrl_handle)0)

/* Result codes for host ops. 0 == success; negative == failure. Not HRESULTs:
 * the host maps native HRESULTs to these; the precise HRESULT of the last
 * failing gfx call is available via xrl_gfx_ops.last_hresult (v2). */
typedef int32_t xrl_status;
#define XRL_OK             ((xrl_status)0)
#define XRL_ERR_GENERIC    ((xrl_status)-1)
#define XRL_ERR_NOTAVAIL   ((xrl_status)-2)   /* feature absent on this console */
#define XRL_ERR_DEVICELOST ((xrl_status)-3)

/* v2: adapter identity for Wine's dxgi (IDXGIAdapter::GetDesc etc.). */
typedef struct xrl_adapter_info {
    uint32_t struct_size;             /* sizeof(xrl_adapter_info)             */
    uint32_t vendor_id, device_id, subsys_id, revision;
    uint32_t max_feature_level;       /* D3D_FEATURE_LEVEL, e.g. 0xb000 = 11_0 */
    uint64_t dedicated_video_memory;  /* bytes                                */
    uint64_t dedicated_system_memory;
    uint64_t shared_system_memory;
    uint64_t adapter_luid;            /* LowPart | ((uint64_t)HighPart << 32) */
    uint16_t description[128];        /* UTF-16, NUL-terminated               */
} xrl_adapter_info;

/*
 * Graphics forwarding. The host owns the CoreWindow and creates devices and
 * swapchains on its gfx thread (D3D creation on the Xbox UI thread hangs).
 * The guest creates its own direct command queue on the device (a COM call)
 * and passes that queue to create_swapchain_for_corewindow and
 * d3d11on12_create; queue must be non-NULL.
 */
typedef struct xrl_gfx_ops {
    uint32_t struct_size;             /* sizeof(xrl_gfx_ops) for versioning   */

    xrl_status (*d3d12_create_device)(xrl_handle *out_device);
    xrl_status (*create_swapchain_for_corewindow)(xrl_handle device,
                                                  xrl_handle queue,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t buffer_count,
                                                  uint32_t dxgi_format,
                                                  xrl_handle *out_swapchain);
    xrl_status (*swapchain_get_buffer)(xrl_handle swapchain, uint32_t index,
                                       xrl_handle *out_resource);
    xrl_status (*swapchain_present)(xrl_handle swapchain, uint32_t sync_interval,
                                    uint32_t flags);
    /* NULL when D3D11On12 is unavailable (host preflight failed). */
    xrl_status (*d3d11on12_create)(xrl_handle d3d12_device,
                                   xrl_handle d3d12_queue,
                                   xrl_handle *out_d3d11_device,
                                   xrl_handle *out_d3d11_context);
    void       (*release)(xrl_handle h);

    /* ---- v2 (append-only; check struct_size before use) ---- */
    xrl_status (*swapchain_get_current_index)(xrl_handle swapchain,
                                              uint32_t *out_index);
    /* 0 = keep current value for each argument. Release all buffer refs first. */
    xrl_status (*swapchain_resize)(xrl_handle swapchain, uint32_t buffer_count,
                                   uint32_t width, uint32_t height,
                                   uint32_t dxgi_format);
    /* HRESULT behind the most recent failing gfx op on THIS thread; 0 if none. */
    int32_t    (*last_hresult)(void);
    xrl_status (*get_adapter_info)(xrl_adapter_info *out);
} xrl_gfx_ops;

/* Audio forwarding -> native XAudio2 (xaudio2_9.dll on the console). */
typedef struct xrl_audio_ops {
    uint32_t struct_size;
    xrl_status (*xaudio2_create)(xrl_handle *out_xaudio2);
    xrl_status (*create_mastering_voice)(xrl_handle xaudio2, uint32_t channels,
                                         uint32_t sample_rate,
                                         xrl_handle *out_voice);
    /* bits_per_sample 32 means IEEE float; 16 means PCM int16. */
    xrl_status (*create_source_voice)(xrl_handle xaudio2,
                                      uint32_t channels, uint32_t sample_rate,
                                      uint32_t bits_per_sample,
                                      xrl_handle *out_voice);
    /* flags: 0, or XRL_AUDIO_END_OF_STREAM. The host copies `data`. */
    xrl_status (*voice_submit_buffer)(xrl_handle voice, const void *data,
                                      uint32_t bytes, uint32_t flags);
    xrl_status (*voice_start)(xrl_handle voice);
    xrl_status (*voice_stop)(xrl_handle voice);
    void       (*release)(xrl_handle h);

    /* ---- v2 ---- */
    /* Pacing for a WASAPI-style driver: frames consumed and buffers in flight. */
    xrl_status (*voice_get_state)(xrl_handle voice, uint64_t *samples_played,
                                  uint32_t *buffers_queued);
} xrl_audio_ops;
#define XRL_AUDIO_END_OF_STREAM 0x40u   /* == XAUDIO2_END_OF_STREAM */

/* Input forwarding -> native Windows.Gaming.Input. Keyboard/mouse: not yet. */
typedef struct xrl_pad_state {
    uint32_t buttons;        /* XINPUT-compatible button bitmask              */
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    uint8_t  connected;      /* 0/1                                           */
    uint8_t  _pad[3];
} xrl_pad_state;

typedef struct xrl_input_ops {
    uint32_t struct_size;
    /* Poll gamepad `slot` (0..3): XRL_OK + state, or XRL_ERR_NOTAVAIL if empty. */
    xrl_status (*get_pad_state)(uint32_t slot, xrl_pad_state *out);
    xrl_status (*set_pad_vibration)(uint32_t slot, uint16_t left_motor,
                                    uint16_t right_motor);
} xrl_input_ops;

/* ------------------------------------------------------------------ *
 * Seam 2: the boot context  (host/ -> layer/)
 * ------------------------------------------------------------------ */

/*
 * Host logging callback; everything lands in LocalState/log.txt in order.
 * level: 0=trace 1=info 2=warn 3=error. `msg` is formatted UTF-8.
 * MUST be thread-safe.
 */
typedef void (*xrl_log_fn)(int level, const char *tag, const char *msg);

typedef struct xrl_boot_context {
    uint32_t struct_size;             /* sizeof(xrl_boot_context)             */
    uint32_t abi_version;             /* XRL_ABI_VERSION the host built with  */

    /* Filesystem roots, UTF-8. vfs_root is a host (Win32) path under writable
     * LocalState; prefix_path/game_path are guest paths inside it. */
    const char *vfs_root;
    const char *prefix_path;
    const char *game_path;

    /* argv/envp for the guest, NULL-terminated. argv[0] is the guest path of
     * the ELF to load unless exec_path (v2) is set. */
    const char *const *argv;
    const char *const *envp;

    xrl_log_fn            log;
    const xrl_gfx_ops    *gfx;        /* never NULL                            */
    const xrl_audio_ops  *audio;      /* never NULL                            */
    const xrl_input_ops  *input;      /* never NULL                            */

    /* COMMITTED-memory budget, bytes. Measured on the Series S (Game bucket):
     * commits above ~4.5 GiB fail with ERROR_COMMITMENT_LIMIT, while reserving
     * address space succeeds up to 1 TiB. The layer reserves freely and keeps
     * commit under this budget. */
    uint64_t guest_mem_budget;

    /* ---- v2 ---- */
    /* Guest path of the ELF to load; NULL = argv[0]. Lets argv[0] be what the
     * guest should see (e.g. "wine64") independent of the file loaded. */
    const char *exec_path;
} xrl_boot_context;

/*
 * The layer's single entry point. Maps the ELF, wires xrl_syscall, starts the
 * guest. Returns the guest's exit code, or a negative xrl_status if the image
 * could not be brought up. Blocks until the guest exits. Single-shot: call at
 * most once per process. Accepts abi_version 1 or 2 (reads v2 fields only when
 * struct_size covers them).
 */
int xrl_layer_boot(const xrl_boot_context *ctx);

/* Ask a running layer to begin orderly shutdown. Safe from any thread. */
void xrl_layer_request_exit(int exit_code);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* XRL_ABI_H */
