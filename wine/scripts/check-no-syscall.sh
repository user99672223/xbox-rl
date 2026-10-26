#!/usr/bin/env bash
# check-no-syscall.sh <file-or-dir>...
#
# The acceptance test for "the output contains NO `syscall` instructions".
# Disassembles every ELF object/archive/shared object (and every PE image) found
# under the given paths and fails if it finds a `syscall`, `sysenter`, `int $0x80`
# or `int $0x2e` instruction.  Mnemonic-aware (objdump -d), so function names
# like <syscall> or the string "syscall" do not trigger it.
#
# PE images are disassembled with the mingw objdump; every Nt* stub in Wine's
# PE ntdll/win32u used to contain `syscall` and is covered by patch 0001.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need objdump file find awk

bad=0; checked=0
re='^[[:space:]]*[0-9a-f]+:[[:space:]]+(syscall|sysenter|sysret[a-z]*|int[[:space:]]+\$0x(80|2e))([[:space:]]|$)'

scan_one() {  # file objdump
  local f=$1 od=$2 hits
  hits=$("$od" -d --no-show-raw-insn "$f" 2>/dev/null | grep -En "$re" | head -5 || true)
  checked=$((checked+1))
  if [ -n "$hits" ]; then
    bad=$((bad+1)); warn "syscall instruction(s) in $f"; printf '    %s\n' $hits | head -5 >&2
  fi
}

for p in "$@"; do
  while IFS= read -r -d '' f; do
    t=$(file -b "$f")
    case "$t" in
      ELF*x86-64*|*"current ar archive"*) scan_one "$f" objdump ;;
      PE32+*) scan_one "$f" x86_64-w64-mingw32-objdump ;;
    esac
  done < <(find "$p" -type f -print0)
done

log "check-no-syscall: scanned $checked binaries, $bad with syscall instructions"
[ "$bad" = 0 ]
