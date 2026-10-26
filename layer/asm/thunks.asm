; thunks.asm - System V <-> Win64 transition thunks for the xbox-rl layer.
;
; Target: MASM (ml64) for x64 Windows / UWP. This file contains the only
; hand-written machine code in the layer.
;
; Contract summary
;   xrl_syscall          SysV entry the guest calls instead of `syscall`.
;                        Saves SysV callee-saved regs, switches to the thread's
;                        host stack, calls xrl_syscall_dispatch (Win64), switches
;                        back, re-arms FS base, returns the Linux result in rax.
;   xrl_downcall_tramp   Generic SysV->Win64 call for host op-table functions.
;                        Target Win64 function pointer is passed in r11 (the
;                        per-op stubs generated at runtime load it).
;   xrl_enter_guest      Win64: start a guest thread at `entry` on `guest_rsp`.
;   xrl_clone_resume     Win64: start a cloned thread: restore the parent's
;                        SysV callee-saved regs, rsp=child stack, rax=0 and
;                        "return" from the xrl_syscall call site.
;
; Finding the calling thread's record: gs:[30h] is the host TEB's Self pointer.
; It is hashed into xrl_ttab (open addressing, 16-byte entries {teb, thread*},
; key 0 = empty, key 1 = tombstone). We deliberately do NOT use TlsSlots or any
; other TEB field: the guest (Wine) shares the TEB and will scribble on them.
;
; Thread record offsets (must match struct xrl_thread, asserted in thread.c):
;   +00 host_rsp   +08 guest_tp   +10 frame   +18 self
;   +20 resume[8]  +60 child_rsp  +68 fsb_enabled

OPTION CASEMAP:NONE

EXTERN xrl_syscall_dispatch:PROC
EXTERN xrl_teb_enter_host:PROC
EXTERN xrl_teb_restore:PROC
EXTERN xrl_com_scan:PROC
EXTERN xrl_com_after:PROC
EXTERN xrl_ttab:BYTE
EXTERN xrl_asm_fsb_flag:QWORD

T_HOST_RSP   EQU 00h
T_GUEST_TP   EQU 08h
T_FRAME      EQU 10h
T_RESUME     EQU 20h
T_CHILD_RSP  EQU 60h
T_GUEST_LIVE EQU 70h

; FIND_THREAD: rax = xrl_thread* (0 if unknown). Clobbers r12, r13, r10.
FIND_THREAD MACRO
    LOCAL probe, found, wrap, none, done
    mov     rax, gs:[30h]
    mov     r12, rax
    shr     r12, 12
    and     r12d, 4095
    shl     r12, 4
    lea     r13, xrl_ttab
    add     r12, r13
probe:
    mov     r10, [r12]
    cmp     r10, rax
    je      short found
    test    r10, r10
    jz      short none
    add     r12, 16
    lea     r10, [r13+65536]
    cmp     r12, r10
    jb      short probe
    mov     r12, r13
    jmp     short probe
found:
    mov     rax, [r12+8]
    jmp     short done
none:
    xor     eax, eax
done:
ENDM

.code

; --------------------------------------------------------------------------
; long xrl_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
;   SysV: rdi=nr rsi=a1 rdx=a2 rcx=a3 r8=a4 r9=a5 [rsp+8]=a6
; Saved SysV frame (frame pointer = rbp-40), low -> high:
;   +00 r15 +08 r14 +10 r13 +18 r12 +20 rbx +28 rbp +30 retaddr +38 a6
; --------------------------------------------------------------------------
PUBLIC xrl_syscall
xrl_syscall PROC
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    FIND_THREAD                            ; rax = thread*
    test    rax, rax
    jz      sc_nothread
    mov     rbx, rax                       ; rbx = thread* (callee-saved in Win64)

    vzeroupper
    lea     r11, [rbp-40]                  ; frame
    mov     [rbx+T_FRAME], r11
    mov     r10, [rbx+T_HOST_RSP]
    mov     rsp, r10                       ; ---- now on the host stack ----
    sub     rsp, 40h
    mov     [rsp+20h], r8                  ; a4
    mov     [rsp+28h], r9                  ; a5
    mov     r10, [rbp+16]                  ; a6 (from guest stack)
    mov     [rsp+30h], r10
    mov     [rsp+38h], r11                 ; frame pointer
    mov     r9, rcx                        ; a3
    mov     r8, rdx                        ; a2
    mov     rdx, rsi                       ; a1
    mov     rcx, rdi                       ; nr
    call    xrl_syscall_dispatch           ; Win64 -> rax

    ; re-arm the guest's FS base: Windows zeroes it on every context switch,
    ; so the first guest access after a blocking syscall would otherwise take
    ; a (recoverable) fault. Cheap insurance.
    cmp     qword ptr [xrl_asm_fsb_flag], 0
    je      short sc_done
    mov     rcx, [rbx+T_GUEST_TP]
    test    rcx, rcx
    jz      short sc_done
    db      0F3h, 48h, 0Fh, 0AEh, 0D1h    ; wrfsbase rcx
sc_done:
    lea     rsp, [rbp-40]                  ; ---- back on the guest stack ----
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

sc_nothread:
    mov     rax, -38                       ; -ENOSYS: unregistered thread
    jmp     short sc_done
xrl_syscall ENDP

; --------------------------------------------------------------------------
; Generic SysV -> Win64 downcall. r11 = Win64 target. Up to 8 integer args
; (7th/8th on the guest stack at [rsp+8]/[rsp+16] on entry).
; --------------------------------------------------------------------------
PUBLIC xrl_downcall_tramp
xrl_downcall_tramp PROC
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r12
    push    r13
    mov     rbx, r11                       ; target (callee-saved across FIND_THREAD)
    FIND_THREAD
    mov     r12, rax                       ; xrl_thread* or 0 (callee-saved from here on)
    mov     r10, rsp
    test    rax, rax
    jz      short dc_havestack
    vzeroupper
    mov     r10, [rax+T_HOST_RSP]
dc_havestack:
    and     r10, -16
    mov     rsp, r10
    sub     rsp, 0A0h                      ; 20h shadow, 20h stack args, 40h SysV args, 20h result
    ; Host code runs below: swap the host TEB view in (docs/gs-teb.md). The Win64
    ; helper may clobber rcx/rdx/r8/r9, which still hold SysV args 4/3/5/6.
    ; All eight SysV args are kept at [rsp+40h] in order, for the COM scan.
    mov     [rsp+40h], rdi
    mov     [rsp+48h], rsi
    mov     [rsp+50h], rdx
    mov     [rsp+58h], rcx
    mov     [rsp+60h], r8
    mov     [rsp+68h], r9
    mov     rax, [rbp+16]                  ; sysv arg7
    mov     r10, [rbp+24]                  ; sysv arg8
    mov     [rsp+70h], rax
    mov     [rsp+78h], r10
    mov     rcx, r12
    call    xrl_teb_enter_host             ; eax = 1 if the guest view was live
    mov     r13d, eax                      ; restore flag (callee-saved)
    mov     rax, [rsp+70h]
    mov     r10, [rsp+78h]
    mov     [rsp+30h], rax
    mov     [rsp+38h], r10
    mov     rax, [rsp+60h]
    mov     [rsp+20h], rax                 ; arg5
    mov     rax, [rsp+68h]
    mov     [rsp+28h], rax                 ; arg6
    mov     r9, [rsp+58h]
    mov     r8, [rsp+50h]
    mov     rdx, [rsp+48h]
    mov     rcx, [rsp+40h]
    call    rbx
    mov     [rsp+80h], rax                 ; native return value
    lea     rcx, [rsp+40h]                 ; out-params may carry host COM objects
    mov     edx, 8
    call    xrl_com_scan
    mov     rcx, r12
    mov     edx, r13d
    call    xrl_teb_restore                ; guest view back if it was live
    mov     rax, [rsp+80h]
    lea     rsp, [rbp-24]
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret
xrl_downcall_tramp ENDP

; --------------------------------------------------------------------------
; xrl_com_tramp: Win64 -> Win64, reached from the per-vtable-entry stubs of
; com.c with r11 = the real method. Arguments are untouched Win64 ones.
;   - thread unknown, or host TEB view live: jump straight to the method.
;   - guest view live (called from guest code): host stack, host TEB view,
;     call, scan the arguments for new host COM objects, guest view back.
; Copies 12 stack arguments (methods with up to 16 arguments); xmm0-3 are
; preserved for float arguments, and xmm0 for a float result.
; Host frame (rsp 16-aligned):
;   +00 shadow  +20 stack args 5..16  +80 rcx rdx r8 r9  +A0 xmm0-3
;   +E0 rax result  +F0 xmm0 result
; Guest frame: rbp -> saved rbp; +08 r13, +10 r12, +18 return, +40 arg 5.
; --------------------------------------------------------------------------
PUBLIC xrl_com_tramp
xrl_com_tramp PROC
    push    r12
    push    r13
    FIND_THREAD                            ; rax = thread* (clobbers r10 r12 r13)
    test    rax, rax
    jz      ct_fast
    cmp     dword ptr [rax+T_GUEST_LIVE], 0
    je      ct_fast
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    r14
    push    r15
    mov     rbx, r11                       ; target
    mov     r12, rax                       ; thread
    mov     r10, [rax+T_HOST_RSP]
    and     r10, -16
    mov     rsp, r10                       ; ---- now on the host stack ----
    sub     rsp, 100h
    mov     [rsp+80h], rcx
    mov     [rsp+88h], rdx
    mov     [rsp+90h], r8
    mov     [rsp+98h], r9
    movdqu  [rsp+0A0h], xmm0
    movdqu  [rsp+0B0h], xmm1
    movdqu  [rsp+0C0h], xmm2
    movdqu  [rsp+0D0h], xmm3
    xor     ecx, ecx
ct_copy:
    mov     rax, [rbp+40h+rcx*8]
    mov     [rsp+20h+rcx*8], rax
    inc     ecx
    cmp     ecx, 12
    jb      ct_copy
    mov     rcx, r12
    call    xrl_teb_enter_host             ; eax = 1 if the guest view was live
    mov     r15d, eax
    mov     rcx, [rsp+80h]
    mov     rdx, [rsp+88h]
    mov     r8,  [rsp+90h]
    mov     r9,  [rsp+98h]
    movdqu  xmm0, [rsp+0A0h]
    movdqu  xmm1, [rsp+0B0h]
    movdqu  xmm2, [rsp+0C0h]
    movdqu  xmm3, [rsp+0D0h]
    call    rbx
    mov     [rsp+0E0h], rax
    movdqu  [rsp+0F0h], xmm0
    lea     rcx, [rsp+80h]                 ; regs as passed
    lea     rdx, [rbp+40h]                 ; stack args as passed (guest copy)
    mov     r8, rax
    call    xrl_com_after
    mov     rcx, r12
    mov     edx, r15d
    call    xrl_teb_restore
    mov     rax, [rsp+0E0h]
    movdqu  xmm0, [rsp+0F0h]
    lea     rsp, [rbp-24]                  ; ---- back on the guest stack ----
    pop     r15
    pop     r14
    pop     rbx
    pop     rbp
    pop     r13
    pop     r12
    ret
ct_fast:
    pop     r13
    pop     r12
    jmp     r11
xrl_com_tramp ENDP

; --------------------------------------------------------------------------
; void xrl_enter_guest(u64 entry /*rcx*/, u64 guest_rsp /*rdx*/, xrl_thread *t /*r8*/)
; Never returns. Establishes the thread's host stack top just below this
; frame, then jumps into the guest with a Linux-style clean register file.
; --------------------------------------------------------------------------
PUBLIC xrl_enter_guest
xrl_enter_guest PROC
    mov     rax, rsp
    sub     rax, 80h
    and     rax, -16
    mov     [r8+T_HOST_RSP], rax
    mov     r11, rcx
    mov     r10, [r8+T_GUEST_TP]
    mov     rsp, rdx
    cmp     qword ptr [xrl_asm_fsb_flag], 0
    je      short eg_nofs
    test    r10, r10
    jz      short eg_nofs
    db      0F3h, 49h, 0Fh, 0AEh, 0D2h     ; wrfsbase r10
eg_nofs:
    fninit                                  ; x87 CW = 0x37F like Linux
    cld
    xor     eax, eax
    xor     ebx, ebx
    xor     ecx, ecx
    xor     edx, edx                        ; rdx = 0 (atexit ptr for _start)
    xor     esi, esi
    xor     edi, edi
    xor     ebp, ebp
    xor     r8d, r8d
    xor     r9d, r9d
    xor     r10d, r10d
    xor     r12d, r12d
    xor     r13d, r13d
    xor     r14d, r14d
    xor     r15d, r15d
    jmp     r11
xrl_enter_guest ENDP

; --------------------------------------------------------------------------
; void xrl_clone_resume(xrl_thread *t /*rcx*/)
; Child side of clone(): behave as if `syscall` returned 0 on the new stack.
; --------------------------------------------------------------------------
PUBLIC xrl_clone_resume
xrl_clone_resume PROC
    mov     rax, rsp
    sub     rax, 80h
    and     rax, -16
    mov     [rcx+T_HOST_RSP], rax
    mov     rdx, [rcx+T_GUEST_TP]
    cmp     qword ptr [xrl_asm_fsb_flag], 0
    je      short cr_nofs
    test    rdx, rdx
    jz      short cr_nofs
    db      0F3h, 48h, 0Fh, 0AEh, 0D2h     ; wrfsbase rdx
cr_nofs:
    fninit
    cld
    mov     r15, [rcx+T_RESUME+00h]
    mov     r14, [rcx+T_RESUME+08h]
    mov     r13, [rcx+T_RESUME+10h]
    mov     r12, [rcx+T_RESUME+18h]
    mov     rbx, [rcx+T_RESUME+20h]
    mov     rbp, [rcx+T_RESUME+28h]
    mov     r11, [rcx+T_RESUME+30h]         ; return address of the xrl_syscall call
    mov     rsp, [rcx+T_CHILD_RSP]
    xor     eax, eax                        ; clone() returns 0 in the child
    jmp     r11
xrl_clone_resume ENDP

END
