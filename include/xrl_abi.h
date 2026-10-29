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

/* v3 (2026-10-27): keyboard/mouse event queue in xrl_input_ops, present_framebuffer in xrl_gfx_ops. */
#define XRL_ABI_VERSION 3u

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
/* Added later in v2 (additive: an older layer answers -ENOSYS and libc keeps the syscall):
 * a SysV-callable int (*)(clockid_t, struct timespec *) with the kernel vDSO's contract (0, or
 * -errno; -ENOSYS for a clock it does not serve). musl's clock_gettime calls it directly instead of
 * entering the layer: no host stack, no TEB view swap (patches/musl/0004). */
#define XRL_SYS_GET_VCLOCK     0x10005

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
    X(0x800, 9)   /* Win32ClientInfo: Wine's ntuser_thread_info (desktop  */ \
                  /* window handles), win32k's CLIENTINFO for the host    */ \
    X(0x1250, 1)  /* LastStatusValue                                      */ \
    X(0x1478, 64) /* DeallocationStack, TlsSlots[0..62]                   */ \
    X(0x1758, 1)  /* ReservedForOle (COM / WinRT apartment state)         */ \
    X(0x1778, 2)  /* ThreadPoolData, TlsExpansionSlots                    */ \
    X(0x17a0, 1)  /* NlsCache                                             */ \
    X(0x17c8, 5)  /* FlsData, Preferred/User/MergedPrefLanguages, flags   */
#define XRL__TEB_SUM(off, n) + (n)
#define XRL_TEB_SWAP_QWORDS (0 XRL_TEB_SWAP_RANGES(XRL__TEB_SUM))   /* 100 */

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

/* v2: what a D3D11 game asked for in DXGI_SWAP_CHAIN_DESC / DXGI_SWAP_CHAIN_DESC1
 * (same numeric values as the DXGI enums). Zero width/height = window size. */
typedef struct xrl_swapchain_desc {
    uint32_t struct_size;             /* sizeof(xrl_swapchain_desc)           */
    uint32_t width, height;
    uint32_t buffer_count;
    uint32_t dxgi_format;
    uint32_t sample_count, sample_quality;
    uint32_t buffer_usage;            /* DXGI_USAGE_*                         */
    uint32_t swap_effect;             /* DXGI_SWAP_EFFECT_*                   */
    uint32_t flags;                   /* DXGI_SWAP_CHAIN_FLAG_*               */
    uint32_t windowed;
    uint32_t reserved;
    uint64_t hwnd;                    /* the guest's HWND; reported by GetHwnd only */
} xrl_swapchain_desc;

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

    /* ---- v2, appended after the first presented frame ---- */
    /* Swapchain for a D3D11 game: `d3d11_device` is the ID3D11Device* that
     * d3d11on12_create returned, `d3d12_queue` the queue it was made with. The
     * result is NOT the DXGI swapchain of the OS but an IDXGISwapChain1 object
     * made by gfx/ that the guest calls directly: GetBuffer(0, ID3D11Texture2D)
     * returns one stable D3D11 texture, Present copies it into the current
     * buffer of a flip-model D3D12 swapchain on the host window and presents
     * (the D3D11 swapchain contract on top of D3D11On12). Release it with
     * `release`. */
    xrl_status (*d3d11_create_swapchain)(xrl_handle d3d11_device,
                                         xrl_handle d3d12_queue,
                                         const struct xrl_swapchain_desc *desc,
                                         xrl_handle *out_swapchain);

    /* ---- v3 ---- */
    /* Show a framebuffer in the window: `pixels` is guest memory, 32 bits per pixel in B,G,R,X byte
     * order (DXGI B8G8R8X8; the X byte is ignored), `stride` in bytes (>= width * 4). The image is scaled
     * to fit the window with its aspect ratio kept (black bars). The host owns the window's swapchain for
     * this (do not mix with the guest's own swapchain), keeps its own copy of the pixels before returning,
     * and paces to the display: flags 0 waits for vblank (one frame per refresh at most),
     * XRL_PRESENT_NOWAIT does not. Any thread; calls are serialized. */
    xrl_status (*present_framebuffer)(const void *pixels, uint32_t width, uint32_t height,
                                      uint32_t stride, uint32_t flags);
} xrl_gfx_ops;
#define XRL_PRESENT_NOWAIT 1u

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

/* Input forwarding: gamepads -> native Windows.Gaming.Input; keyboard and mouse -> an event queue the
 * host fills from the window system (SDL/CoreWindow on the console, a Win32 window on the test host). */
typedef struct xrl_pad_state {
    uint32_t buttons;        /* XINPUT-compatible button bitmask              */
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    uint8_t  connected;      /* 0/1                                           */
    uint8_t  _pad[3];
} xrl_pad_state;

/* v3: one keyboard/mouse event. The vocabulary is Linux evdev's on purpose (linux/input-event-codes.h
 * numbers): the layer's /dev/input/eventN passes events through nearly verbatim, and Wine maps the
 * same codes to PS/2 scancodes for SendInput. */
typedef struct xrl_input_event {
    uint16_t type;           /* XRL_IEV_*                                     */
    uint16_t code;           /* key/button code, or axis                      */
    int32_t  value;
    uint64_t time_ns;        /* host monotonic clock                          */
} xrl_input_event;
enum {
    /* code = KEY_* (0..255) or BTN_* (BTN_LEFT 0x110, BTN_RIGHT 0x111, BTN_MIDDLE 0x112, BTN_SIDE 0x113,
     * BTN_EXTRA 0x114); value 0 = release, 1 = press, 2 = autorepeat. The host drops a press of a key it
     * already holds (the queue turns it into 2) and releases every held key when the window loses focus. */
    XRL_IEV_KEY   = 1,
    /* Relative motion, only while the mouse mode is XRL_MOUSE_RELATIVE: code = REL_X 0, REL_Y 1,
     * REL_HWHEEL 6, REL_WHEEL 8; value = counts (device pixels), or notches for the wheels (+up/+right).
     * In XRL_MOUSE_ABSOLUTE the wheels are still sent as REL events; X/Y are not. */
    XRL_IEV_REL   = 2,
    /* Absolute pointer position while the mouse mode is XRL_MOUSE_ABSOLUTE: code = ABS_X 0, ABS_Y 1;
     * value 0..XRL_ABS_MAX over the content area (the whole window, or the aspect-fitted picture once
     * the guest called set_content_size). The pointer is outside the content area in the bars: clamped. */
    XRL_IEV_ABS   = 3,
    /* Text input: value = a Unicode code point (the OS keyboard layout's translation of the key presses). */
    XRL_IEV_TEXT  = 4,
    /* Window focus: value 1 gained, 0 lost (held keys were released just before). */
    XRL_IEV_FOCUS = 5,
};
#define XRL_ABS_MAX 32767
#define XRL_MOUSE_ABSOLUTE 0u    /* cursor visible, ABS events (default)                       */
#define XRL_MOUSE_RELATIVE 1u    /* cursor hidden and captured by the window, REL events        */

typedef struct xrl_input_ops {
    uint32_t struct_size;
    /* Poll gamepad `slot` (0..3): XRL_OK + state, or XRL_ERR_NOTAVAIL if empty. */
    xrl_status (*get_pad_state)(uint32_t slot, xrl_pad_state *out);
    xrl_status (*set_pad_vibration)(uint32_t slot, uint16_t left_motor,
                                    uint16_t right_motor);

    /* ---- v3 (append-only; check struct_size before use) ---- */
    /* The event queue has ONE consumer (the layer's evdev devices, or Wine's xrlhost; not both at
     * once): events are removed as they are read. */
    /* Non-blocking: copy up to `max` queued events to `buf`, oldest first; returns how many. */
    uint32_t   (*read_events)(xrl_input_event *buf, uint32_t max);
    /* Block until at least one event is queued: XRL_OK, or XRL_ERR_NOTAVAIL after `timeout_ms`
     * (0xFFFFFFFF = no timeout). Any thread; several waiters are all released. */
    xrl_status (*wait_events)(uint32_t timeout_ms);
    /* XRL_MOUSE_ABSOLUTE / XRL_MOUSE_RELATIVE (the host shows/hides and captures the cursor). */
    xrl_status (*set_mouse_mode)(uint32_t mode);
    /* The picture the guest shows (present_framebuffer's size), so ABS events can be mapped onto the
     * aspect-fitted image rather than the whole window. 0,0 = the whole window. */
    xrl_status (*set_content_size)(uint32_t width, uint32_t height);
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
 * most once per process. Accepts abi_version 1, 2 or 3 (reads v2 fields only when
 * struct_size covers them).
 */
int xrl_layer_boot(const xrl_boot_context *ctx);

/* Ask a running layer to begin orderly shutdown. Safe from any thread. */
void xrl_layer_request_exit(int exit_code);

/*
 * Exec hand-over (XRL_EXEC_HOST=1), called by the first process INSTEAD of
 * booting argv itself. Starts argv as a child host process (command line
 * `<exe> <vfs_root> <argv...>`, envp as XRL_ENV_ lines) and waits. When the
 * child's first traced execve arrives, this process takes it over: its
 * environment becomes that program's (XRL_ENV_*, XRL_TRACEE_R/W, ...) and the
 * function returns 1 with *argv_out (malloc'd, NULL-terminated UTF-8) for the
 * host to boot as a spawned child would. Returns 0 with *exit_code when the
 * child ended without handing over, <0 when it could not be started.
 */
int xrl_exec_host_run(const char *vfs_root, const char *const *argv, const char *const *envp,
                      xrl_log_fn log, char ***argv_out, int *exit_code);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* XRL_ABI_H */
