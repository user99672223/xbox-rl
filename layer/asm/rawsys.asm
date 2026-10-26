; rawsys.asm - what a rewritten `syscall` instruction calls (layer/src/rewrite.c).
;
; The rewriter turns every `syscall` of a foreign (non-xbox-rl) ELF image into a
; stub that does
;       lea  -128(%rsp),%rsp         ; step over the red zone
;       call *slot(%rip)             ; slot -> xrl_rawsys
;       lea  128(%rsp),%rsp          ; always the 8-byte disp32 form
;       ...                          ; the displaced neighbours, then jmp back
; so on entry rax = Linux syscall number, rdi/rsi/rdx/r10/r8/r9 = a1..a6, the
; stack is the guest's (any alignment) and [rsp] is the stub's return address.
;
; Linux preserves every register except rax (result), rcx and r11, and also all
; SSE state; `xrl_syscall` (SysV, thunks.asm) is an ordinary C-ABI call, so the
; argument registers and xmm0-15 are saved here and restored on return.
;
; clone (56): the child must continue after the syscall instruction on its new
; stack with the parent's registers. xrl_syscall's clone takes an explicit
; resume address (a6), so we pass xrl_rawsys_clone_child and leave the state it
; needs just below the new stack pointer (the child reads it before touching
; the stack). The resume point is the return address + 8: the `lea 128(%rsp)`
; of the stub must not run in the child, whose rsp is already exact.
; clone3 (435) answers -ENOSYS like an old kernel, so glibc falls back to clone.

OPTION CASEMAP:NONE

EXTERN xrl_syscall:PROC

.code

PUBLIC xrl_rawsys
xrl_rawsys PROC
    push    rbp
    mov     rbp, rsp
    push    rdi                            ; rbp-8
    push    rsi                            ; rbp-16
    push    rdx                            ; rbp-24
    push    r8                             ; rbp-32
    push    r9                             ; rbp-40
    push    r10                            ; rbp-48
    cmp     rax, 435
    je      rs_clone3
    and     rsp, -16
    sub     rsp, 272                       ; [rsp] a6, [rsp+16..272) xmm0-15
    movups  xmmword ptr [rsp+16],  xmm0
    movups  xmmword ptr [rsp+32],  xmm1
    movups  xmmword ptr [rsp+48],  xmm2
    movups  xmmword ptr [rsp+64],  xmm3
    movups  xmmword ptr [rsp+80],  xmm4
    movups  xmmword ptr [rsp+96],  xmm5
    movups  xmmword ptr [rsp+112], xmm6
    movups  xmmword ptr [rsp+128], xmm7
    movups  xmmword ptr [rsp+144], xmm8
    movups  xmmword ptr [rsp+160], xmm9
    movups  xmmword ptr [rsp+176], xmm10
    movups  xmmword ptr [rsp+192], xmm11
    movups  xmmword ptr [rsp+208], xmm12
    movups  xmmword ptr [rsp+224], xmm13
    movups  xmmword ptr [rsp+240], xmm14
    movups  xmmword ptr [rsp+256], xmm15
    mov     [rsp], r9                      ; a6
    cmp     rax, 56
    je      rs_clone
    cmp     rax, 15
    jne     rs_call
    ; rt_sigreturn: the kernel finds the frame through the guest rsp at the `syscall` (the handler's `ret` popped
    ; pretcode, so it points at the ucontext); the layer takes it as an argument. rbp+144 = the stub's lea (128) +
    ; the call's return address (8) + our saved rbp (8) above our frame.
    lea     rdi, [rbp+144]
rs_call:
    mov     r9, r8                         ; a5
    mov     r8, r10                        ; a4
    mov     rcx, rdx                       ; a3
    mov     rdx, rsi                       ; a2
    mov     rsi, rdi                       ; a1
    mov     rdi, rax                       ; nr
    call    xrl_syscall
    movups  xmm0,  xmmword ptr [rsp+16]
    movups  xmm1,  xmmword ptr [rsp+32]
    movups  xmm2,  xmmword ptr [rsp+48]
    movups  xmm3,  xmmword ptr [rsp+64]
    movups  xmm4,  xmmword ptr [rsp+80]
    movups  xmm5,  xmmword ptr [rsp+96]
    movups  xmm6,  xmmword ptr [rsp+112]
    movups  xmm7,  xmmword ptr [rsp+128]
    movups  xmm8,  xmmword ptr [rsp+144]
    movups  xmm9,  xmmword ptr [rsp+160]
    movups  xmm10, xmmword ptr [rsp+176]
    movups  xmm11, xmmword ptr [rsp+192]
    movups  xmm12, xmmword ptr [rsp+208]
    movups  xmm13, xmmword ptr [rsp+224]
    movups  xmm14, xmmword ptr [rsp+240]
    movups  xmm15, xmmword ptr [rsp+256]
rs_out:
    lea     rsp, [rbp-48]
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rsi
    pop     rdi
    pop     rbp
    ret

rs_clone3:
    mov     rax, -38                       ; -ENOSYS
    jmp     rs_out

    ; clone(flags=rdi, stack=rsi, ptid=rdx, ctid=r10, tls=r8): leave the child's
    ; register state below its stack pointer and give xrl_syscall the resume pc.
rs_clone:
    test    rsi, rsi
    jz      rs_clone_go
    mov     r11, [rbp+8]                   ; stub return address
    add     r11, 8                         ; skip `lea 128(%rsp),%rsp`
    mov     [rsi-8],  r11
    mov     r11, [rbp-8]                   ; rdi
    mov     [rsi-16], r11
    mov     [rsi-24], rsi
    mov     r11, [rbp-24]                  ; rdx
    mov     [rsi-32], r11
    mov     r11, [rbp-32]                  ; r8
    mov     [rsi-40], r11
    mov     r11, [rbp-40]                  ; r9
    mov     [rsi-48], r11
    mov     r11, [rbp-48]                  ; r10
    mov     [rsi-56], r11
    mov     r11, [rbp]                     ; the caller's rbp
    mov     [rsi-64], r11
rs_clone_go:
    lea     r11, xrl_rawsys_clone_child
    mov     [rsp], r11                     ; a6 = resume_pc
    jmp     rs_call
xrl_rawsys ENDP

; Child side: rsp = the new stack, rax = 0, rbx/r12-r15 copied from the parent.
PUBLIC xrl_rawsys_clone_child
xrl_rawsys_clone_child PROC
    mov     rdi, [rsp-16]
    mov     rsi, [rsp-24]
    mov     rdx, [rsp-32]
    mov     r8,  [rsp-40]
    mov     r9,  [rsp-48]
    mov     r10, [rsp-56]
    mov     rbp, [rsp-64]
    mov     r11, [rsp-8]
    xor     eax, eax
    jmp     r11
xrl_rawsys_clone_child ENDP

END
