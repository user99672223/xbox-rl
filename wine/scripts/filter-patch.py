#!/usr/bin/env python3
"""filter-patch.py PATCH PATH...  -- keep only the `git diff` sections for PATHs.

Wine's build regenerates some tracked files (server_protocol.h, request_*.h from
server/protocol.def), so `git diff` in the patched tree also shows them. Use this
to cut a new patch down to the files it actually changes.
"""
import sys


def main(patch, keep):
    with open(patch, encoding='utf-8', newline='') as f:
        text = f.read()
    out, cur = [], None
    for chunk in text.split('diff --git ')[1:]:
        path = chunk.split(' ', 1)[0][2:]          # "a/dir/file" -> "dir/file"
        if path in keep:
            out.append('diff --git ' + chunk)
    if not out:
        sys.exit(f'filter-patch: none of {keep} in {patch}')
    with open(patch, 'w', encoding='utf-8', newline='') as f:
        f.write(''.join(out))
    print(f'filter-patch: kept {len(out)} file(s) in {patch}')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], set(sys.argv[2:]))
