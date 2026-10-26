#!/usr/bin/env python3
"""split-patches.py -- developer tool: turn the working-tree edits of a Wine dev checkout into
the ordered, themed patch series in patches/wine/.

Usage:  split-patches.py <dev-tree> <base-commit> <out-dir>

The dev tree is a git checkout of the pinned Wine commit (plus an optional 'autogen baseline'
commit).  Everything changed relative to <base-commit> *inside the whitelist below* is cut into
hunks and each hunk is assigned to a patch by RULES (first match wins).  Generated files
(configure, server_protocol.h, ...) are not in the whitelist on purpose: the build runs
./autogen.sh after the patches are applied.
"""
import os, re, subprocess, sys

dev, base, out = sys.argv[1:4]

WHITELIST = [
    r'^include/wine/asm\.h$',
    r'^dlls/ntdll/Makefile\.in$', r'^dlls/ntdll/unix/',
    r'^server/[^/]+\.c$', r'^server/Makefile\.in$', r'^server/protocol\.def$',
    r'^tools/makedep\.c$', r'^configure\.ac$', r'^include/Makefile\.in$',
    r'^dlls/xrlhost/', r'^dlls/xinput(1_[1-4]|uap)/', r'^dlls/winegstreamer/media-converter/media-converter\.h$', r'^dlls/win32u/opengl\.c$',
    r'^dlls/d3d11/(d3d11_main\.c|Makefile\.in)$', r'^dlls/d3d12/',
    r'^dlls/dxgi/(factory\.c|Makefile\.in)$',
]

PATCHES = [
    ('0001-pe-syscall-thunk',        'PE ntdll/win32u syscall stubs: no `syscall` instruction, call through __wine_syscall_dispatcher'),
    ('0002-unix-syscall-sites',      'unix side: arch_prctl `syscall` sites -> xrl_syscall; WoW64 int $0x80 gates stubbed; syscall-user-dispatch, 0x7ffe1000 page and wine-preloader re-exec removed'),
    ('0003-musl-compat',             'musl/no-EGL build fixes: mallopt/M_PERTURB, <pthread.h> in media-converter.h, SONAME_LIBEGL fallback in win32u/opengl.c'),
    ('0004-runtime-flags-host-usd',  'XRL_* runtime flags; host-owned KUSER_SHARED_DATA; skip wineboot'),
    ('0005-wineserver-thread',       'wineserver as a thread (wineserver.so), no master socket/lock/fork, no fchdir'),
    ('0006-inproc-process',          'CreateProcess without fork/exec: in-process stubs (xrl_stub_process request)'),
    ('0007-xrlhost',                 'new dlls/xrlhost: the PE+unix seam to xrl_gfx/audio/input_ops'),
    ('0008-xinput-forwarder',        'xinput1_1..1_4 answer from xrl_input_ops'),
    ('0009-d3d-forwarders',          'd3d11 / d3d12 / dxgi swapchain forward to the host (D3D11On12)'),
]
IDX = {n: i for i, (n, _) in enumerate(PATCHES)}

# (file regex, hunk-text regex or None, patch name) -- first match wins
RULES = [
    (r'^include/wine/asm\.h$', None, '0001-pe-syscall-thunk'),
    (r'^dlls/ntdll/unix/signal_x86_64\.c$', None, '0002-unix-syscall-sites'),
    (r'^dlls/ntdll/unix/unix_private\.h$', None, '0002-unix-syscall-sites'),
    (r'^dlls/ntdll/unix/loader\.c$', r'xrl_ntdll_dispatcher_slot|HAVE_WINE_PRELOADER', '0002-unix-syscall-sites'),
    (r'^dlls/ntdll/unix/loader\.c$', r'M_PERTURB', '0003-musl-compat'),
    (r'^dlls/ntdll/unix/loader\.c$', r'xrl_init_flags', '0004-runtime-flags-host-usd'),
    (r'^dlls/ntdll/unix/loader\.c$', r'xrl_start_wineserver', '0005-wineserver-thread'),
    (r'^dlls/ntdll/unix/(xrl\.c|virtual\.c|env\.c)$', None, '0004-runtime-flags-host-usd'),
    (r'^dlls/ntdll/Makefile\.in$', None, '0004-runtime-flags-host-usd'),
    (r'^dlls/ntdll/unix/server\.c$', None, '0005-wineserver-thread'),
    (r'^dlls/ntdll/unix/process\.c$', None, '0006-inproc-process'),
    (r'^server/protocol\.def$', None, '0006-inproc-process'),
    (r'^server/process\.c$', r'xrl_stub_process', '0006-inproc-process'),
    (r'^server/[^/]+$', None, '0005-wineserver-thread'),
    (r'^tools/makedep\.c$', None, '0005-wineserver-thread'),
    (r'^configure\.ac$', None, '0007-xrlhost'),
    (r'^include/Makefile\.in$', None, '0007-xrlhost'),
    (r'^dlls/xrlhost/', None, '0007-xrlhost'),
    (r'^dlls/xinput(1_|uap)', None, '0008-xinput-forwarder'),
    (r'^dlls/winegstreamer/media-converter/', None, '0003-musl-compat'),
    (r'^dlls/win32u/opengl\.c$', None, '0003-musl-compat'),
    (r'^dlls/(d3d11|d3d12|dxgi)/', None, '0009-d3d-forwarders'),
]

def git(*a):
    return subprocess.run(['git', '-C', dev] + list(a), check=True, capture_output=True, text=True).stdout

git('add', '-N', '.')
paths = [p for p in git('diff', '--name-only', base).split('\n') if p and any(re.search(w, p) for w in WHITELIST)]
sel = {n: [] for n, _ in PATCHES}

for path in paths:
    d = git('diff', '--no-color', '-U3', base, '--', path)
    # split header / hunks
    parts = re.split(r'(?m)^(?=@@ )', d)
    header, hunks = parts[0], parts[1:]
    if 'new file mode' in header or 'deleted file mode' in header:
        hunks = [''.join(hunks)]          # keep new files in one piece
    for h in hunks:
        for frx, hrx, name in RULES:
            if re.search(frx, path) and (hrx is None or re.search(hrx, h)):
                sel[name].append((path, header, h)); break
        else:
            sys.exit('no rule for %s hunk:\n%s' % (path, h[:300]))

os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    if f.endswith('.patch'): os.remove(os.path.join(out, f))
for name, desc in PATCHES:
    items = sel[name]
    if not items: continue
    byfile = {}
    for path, header, h in items:
        byfile.setdefault(path, [header, []])[1].append(h)
    body = ''.join(hd + ''.join(hs) for hd, hs in byfile.values())
    with open(os.path.join(out, name + '.patch'), 'w') as f:
        f.write(body)
    print('%-34s %3d files  %s' % (name, len(byfile), desc))
