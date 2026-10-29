; dbt.asm - the three indirect-branch helpers of the translator (layer/src/dbt.c).
;
; Translated code (the copy in the code cache) turns
;       jmp *X          into   lea -128(%rsp),%rsp ; push %r11 ; mov X,%r11 ; xchg %r11,(%rsp) ; jmp *ijmp_slot(%rip)
;       call *X         into   mov X,%r11 ; lea cont(%rip),%r10 ; jmp *icall_slot(%rip) ; .quad <original return address> ; cont:
;       ret             into   jmp *ret_slot(%rip)
; All three look the original target up in xrl_dbt_tab (open addressing, 16-byte {original, translated}
; entries, four probes, no wrap; dbt.c inserts under the same rule). A hit continues in the code cache.
; A miss continues at the *original* address: for a native target (musl, Wine) that is simply the call or
; return it always was; for an address of a translated image that has no translation yet it is an
; instruction fetch from a page that is not executable, and the fault handler (dbt.c) translates it and
; puts it in the table, so each target faults once.
;
; Registers: ijmp and ret change only the flags (an indirect jmp can be a switch in the middle of a function,
; and a return path of a local function may be assumed by its caller to leave r10/r11 alone); icall also
; r10 and r11 (an indirect call's callee is an unknown function: the ABI makes both scratch). The red zone
; is respected by the stub of ijmp (it steps over it); call and ret sites have no live red zone.
;
; Return addresses: a call whose target is inside a translated image pushes the ORIGINAL return address
; (what the unmodified program would have pushed: stack walks and the unwinder see addresses of the
; original image); a call to anything else pushes the address of its continuation in the cache, so the
; native callee returns straight into the cache without a fault.

OPTION CASEMAP:NONE

EXTERN xrl_dbt_tab:BYTE
EXTERN xrl_dbt_rng:QWORD
DBT_NRNG EQU 16                    ; translated images the layer can have (dbt.c MAX_IMG): {lo, size} pairs in xrl_dbt_rng
EXTERN xrl_rawsys:PROC

.code

; r11 = target; clobbers r10 and rax. -> r10 = &entry
DBT_HASH MACRO
    mov     rax, 9E3779B97F4A7C15h
    mov     r10, r11
    imul    r10, rax
    shr     r10, 46
    shl     r10, 4
    lea     rax, xrl_dbt_tab
    add     r10, rax
ENDM

PUBLIC xrl_dbt_ijmp
xrl_dbt_ijmp PROC
    ; The stub did: lea -128(%rsp),%rsp ; push %r11 ; mov X,%r11 ; xchg %r11,(%rsp) ; jmp *slot
    ; so [rsp] = the target and every register still has the program's value (an indirect jump may sit in
    ; the middle of a function, where any register is live). Nothing but rax, r10 and the flags is used,
    ; both saved here; the exit is `ret 128h-8` style: it pops the target and drops the pad.
    push    rax
    push    r10
    mov     rax, [rsp+16]
    mov     r10, 9E3779B97F4A7C15h
    imul    r10, rax
    shr     r10, 46
    shl     r10, 4
    lea     rax, xrl_dbt_tab
    add     r10, rax
    mov     rax, [r10]
    cmp     rax, [rsp+16]
    je      ij_h0
    test    rax, rax
    jz      ij_miss
    mov     rax, [r10+16]
    cmp     rax, [rsp+16]
    je      ij_h1
    test    rax, rax
    jz      ij_miss
    mov     rax, [r10+32]
    cmp     rax, [rsp+16]
    je      ij_h2
    test    rax, rax
    jz      ij_miss
    mov     rax, [r10+48]
    cmp     rax, [rsp+16]
    je      ij_h3
ij_miss:
    ; no translation yet. Native target: just go. Target inside a translated image: ask the layer to
    ; translate it (a syscall-style call, not a fault: a fault's exception frame would land on the
    ; red zone, which a leaf function jumping through a switch table may still be using).
    mov     r10, [rsp+16]
    lea     rax, xrl_dbt_rng
RNG_i = 0
    REPT DBT_NRNG
    mov     r10, [rsp+16]
    sub     r10, [rax+RNG_i*16]
    cmp     r10, [rax+RNG_i*16+8]
    jb      ij_enter
RNG_i = RNG_i + 1
    ENDM
    pop     r10
    pop     rax
    ret     80h
ij_enter:
    push    rdi
    push    rcx                            ; rawsys, like the syscall instruction, clobbers rcx and r11
    push    r11
    mov     rdi, [rsp+40]
    mov     eax, 10010h                    ; XRL_SYS_DBT_ENTER: rax = the translation of rdi (or rdi itself)
    call    xrl_rawsys
    mov     [rsp+40], rax
    pop     r11
    pop     rcx
    pop     rdi
    pop     r10
    pop     rax
    ret     80h
ij_h3:
    mov     rax, [r10+56]
    jmp     ij_go
ij_h2:
    mov     rax, [r10+40]
    jmp     ij_go
ij_h1:
    mov     rax, [r10+24]
    jmp     ij_go
ij_h0:
    mov     rax, [r10+8]
ij_go:
    mov     [rsp+16], rax
    pop     r10
    pop     rax
    ret     80h
xrl_dbt_ijmp ENDP

; r11 = target, r10 = continuation in the cache, [r10-8] = original return address
PUBLIC xrl_dbt_icall
xrl_dbt_icall PROC
    push    rax
    push    r10
    DBT_HASH
    mov     rax, [r10]
    cmp     rax, r11
    je      ic_h0
    test    rax, rax
    jz      ic_miss
    mov     rax, [r10+16]
    cmp     rax, r11
    je      ic_h1
    test    rax, rax
    jz      ic_miss
    mov     rax, [r10+32]
    cmp     rax, r11
    je      ic_h2
    test    rax, rax
    jz      ic_miss
    mov     rax, [r10+48]
    cmp     rax, r11
    je      ic_h3
ic_miss:
    ; no translation yet: is the target inside a translated image?
    lea     rax, xrl_dbt_rng
RNG_i = 0
    REPT DBT_NRNG
    mov     r10, r11
    sub     r10, [rax+RNG_i*16]
    cmp     r10, [rax+RNG_i*16+8]
    jb      ic_orig
RNG_i = RNG_i + 1
    ENDM
    ; native target: the return address is the continuation in the cache
    mov     rax, [rsp]
    xchg    rax, [rsp+8]
    lea     rsp, [rsp+8]
    jmp     r11
ic_orig:
    ; a translated image's address without a translation: original return address, enter by faulting
    mov     rax, [rsp]
    mov     rax, [rax-8]
    xchg    rax, [rsp+8]
    lea     rsp, [rsp+8]
    jmp     r11
ic_h3:
    mov     r10, [r10+56]
    jmp     ic_go
ic_h2:
    mov     r10, [r10+40]
    jmp     ic_go
ic_h1:
    mov     r10, [r10+24]
    jmp     ic_go
ic_h0:
    mov     r10, [r10+8]
ic_go:
    mov     rax, [rsp]
    mov     rax, [rax-8]
    xchg    rax, [rsp+8]
    lea     rsp, [rsp+8]
    jmp     r10
xrl_dbt_icall ENDP

; [rsp] = the return address. Nothing changes but the flags: a caller may keep values in r10/r11 across a
; call to a local function (gcc -fipa-ra knows what the callee clobbers), and this helper is in the callee's
; return path. A hit replaces the return address on the stack with its translation and executes `ret`.
PUBLIC xrl_dbt_ret
xrl_dbt_ret PROC
    push    rax
    push    r10
    mov     rax, [rsp+16]
    mov     r10, 9E3779B97F4A7C15h
    imul    r10, rax
    shr     r10, 46
    shl     r10, 4
    lea     rax, xrl_dbt_tab
    add     r10, rax
    mov     rax, [r10]
    cmp     rax, [rsp+16]
    je      rt_h0
    test    rax, rax
    jz      rt_miss
    mov     rax, [r10+16]
    cmp     rax, [rsp+16]
    je      rt_h1
    test    rax, rax
    jz      rt_miss
    mov     rax, [r10+32]
    cmp     rax, [rsp+16]
    je      rt_h2
    test    rax, rax
    jz      rt_miss
    mov     rax, [r10+48]
    cmp     rax, [rsp+16]
    je      rt_h3
rt_miss:
    pop     r10
    pop     rax
    ret
rt_h3:
    mov     rax, [r10+56]
    jmp     rt_go
rt_h2:
    mov     rax, [r10+40]
    jmp     rt_go
rt_h1:
    mov     rax, [r10+24]
    jmp     rt_go
rt_h0:
    mov     rax, [r10+8]
rt_go:
    mov     [rsp+16], rax
    pop     r10
    pop     rax
    ret
xrl_dbt_ret ENDP

END
