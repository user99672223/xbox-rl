#!/usr/bin/env python3
"""xrl-rootfs-unpack.py - unpack a Linux root filesystem tarball into a layer vfs_root.

The layer's VFS (layer/src/vfs.c) cannot use NTFS symlinks or Unix permission bits, so a plain
`tar -x` on Windows loses both. This writes the files and directories as they are and records what NTFS
cannot hold in two tables at the root of the tree:

  .xrl-symlinks   one "guest_path<TAB>target" per symlink (read by vfs.c)
  .xrl-meta       "#default<TAB>0<TAB>0" (the owner of every file not listed: root), then one
                  "guest_path<TAB>mode(octal)<TAB>uid<TAB>gid" per file whose mode/owner is not the layer's
                  default (0755 directory, 0755 file with an executable-looking name like vfs.c is_exec_name,
                  0644 any other file; owner 0:0). The layer appends to this file as the guest chmods/creates.

Hard links become copies. Names Windows cannot represent are stored as U+F0xx (vfs.c comp_to_host).
Device nodes and fifos are skipped (the layer synthesises /dev).

  python -I tools/xrl-rootfs-unpack.py alpine-minirootfs.tar.gz C:\\xrl-work\\alpine\\rootfs
"""
import os
import sys
import tarfile

BAD = set('"*:<>|\\') | {chr(c) for c in range(32)}
EXEC_EXT = ('.exe', '.dll', '.so', '.sh', '.drv', '.sys', '.ocx', '.cpl', '.scr', '.com', '.bat', '.cmd', '.acm', '.ax', '.vxd', '.tlb')


def default_perm(g, isdir):
    return 0o755 if isdir or g.lower().endswith(EXEC_EXT) else 0o644



def host_name(comp):
    out = ''.join(chr(0xF000 + ord(c)) if c in BAD else c for c in comp)
    if out and out[-1] in ' .':
        out = out[:-1] + chr(0xF000 + ord(out[-1]))
    return out


def host_path(root, gpath):
    parts = [host_name(c) for c in gpath.strip('/').split('/') if c and c != '.']
    return '\\\\?\\' + os.path.join(os.path.abspath(root), *parts) if os.name == 'nt' else os.path.join(root, *parts)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tarpath, root = sys.argv[1], sys.argv[2]
    if os.path.exists(root) and os.listdir(root):
        sys.exit('refusing to unpack into a non-empty directory: ' + root)
    os.makedirs(root, exist_ok=True)
    syms, meta, seen, n_files, n_bytes, skipped = {}, {}, {}, 0, 0, []
    if tarpath == '-':
        tf = tarfile.open(fileobj=sys.stdin.buffer, mode='r|*')
    else:
        tf = tarfile.open(tarpath, 'r:*')
    with tf:
        for m in tf:
            g = '/' + m.name.lstrip('./').strip('/')
            if g == '/':
                continue
            low = g.lower()
            if low in seen and seen[low] != g:
                sys.exit('case collision: %s vs %s (the host file system is case-insensitive)' % (seen[low], g))
            seen[low] = g
            hp = host_path(root, g)
            if m.isdir():
                os.makedirs(hp, exist_ok=True)
                if (m.mode & 0o7777) != default_perm(g, True) or m.uid or m.gid:
                    meta[g] = (m.mode & 0o7777, m.uid, m.gid)
            elif m.issym():
                syms[g] = m.linkname
            elif m.islnk():
                src = '/' + m.linkname.lstrip('./').strip('/')
                if src in syms:     # a hard link to a symlink: a symlink
                    syms[g] = syms[src]
                    continue
                os.makedirs(os.path.dirname(hp), exist_ok=True)
                with open(host_path(root, src), 'rb') as a, open(hp, 'wb') as b:
                    b.write(a.read())
                n_files += 1
                if (m.mode & 0o7777) != default_perm(g, False) or m.uid or m.gid:
                    meta[g] = (m.mode & 0o7777, m.uid, m.gid)
            elif m.isreg():
                os.makedirs(os.path.dirname(hp), exist_ok=True)
                with tf.extractfile(m) as a, open(hp, 'wb') as b:
                    while True:
                        buf = a.read(1 << 20)
                        if not buf:
                            break
                        b.write(buf)
                        n_bytes += len(buf)
                n_files += 1
                if (m.mode & 0o7777) != default_perm(g, False) or m.uid or m.gid:
                    meta[g] = (m.mode & 0o7777, m.uid, m.gid)
            else:
                skipped.append(g)
    with open(os.path.join(root, '.xrl-symlinks'), 'w', encoding='utf-8', newline='\n') as f:
        for g in sorted(syms):
            f.write('%s\t%s\n' % (g, syms[g]))
    with open(os.path.join(root, '.xrl-meta'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('#default\t0\t0\n')
        for g in sorted(meta):
            md, u, gi = meta[g]
            f.write('%s\t%o\t%d\t%d\n' % (g, md, u, gi))
    print('%d files (%d bytes), %d symlinks, %d meta entries, %d skipped %s' %
          (n_files, n_bytes, len(syms), len(meta), len(skipped), skipped[:5]))


if __name__ == '__main__':
    main()
