/*
 * TEST-ONLY "layer": a pass-through xrl_syscall that executes the real Linux
 * syscall, so the patched musl can be exercised on the laptop (WSL) without the
 * Xbox.  This file is deliberately NOT part of any shipped binary -- it is the one
 * place a `syscall` instruction is allowed to exist, standing in for the kernel.
 *
 * It implements the three gateway contracts that are not a plain 1:1 syscall:
 *   SYS_rt_sigreturn(15): a1 = &ucontext -> set rsp = a1, issue real sigreturn
 *   SYS_clone(56):        a6 = resume_pc -> child jumps there with rax=0 (a6==0: plain fork-style clone)
 *   SYS_exit(60):         a2/a3 (unmap region) ignored; stack leaks (test only)
 */
	.data
	.align 8
	.global __xrl_gateway
__xrl_gateway:
	.quad test_gateway

	.text
	.type test_gateway,@function
test_gateway:
	cmp $0x10000,%rdi		/* pseudo-syscalls: fake host op-tables (host_ops.c) */
	jb 2f
	jmp xrl_hosttest_pseudo
2:	cmp $15,%rdi
	je .Lsigreturn
	cmp $56,%rdi
	jne .Lgeneric
	cmpq $0,8(%rsp)		/* a6 == 0: plain clone (fork), not the thread contract */
	jne .Lclone
.Lgeneric:
	mov %rdi,%rax
	mov %rsi,%rdi
	mov %rdx,%rsi
	mov %rcx,%rdx
	mov %r8,%r10
	mov %r9,%r8
	mov 8(%rsp),%r9
	syscall
	ret

.Lsigreturn:
	mov %rsi,%rsp
	mov $15,%eax
	syscall
	ud2

.Lclone:
	push %r12
	mov 16(%rsp),%r12	/* a6: resume pc */
	mov %rsi,%rdi		/* flags */
	mov %rdx,%rsi		/* child stack */
	mov %rcx,%rdx		/* ptid */
	mov %r8,%r10		/* ctid */
	mov %r9,%r8		/* tls */
	mov $56,%eax
	syscall
	test %rax,%rax
	jz 1f
	pop %r12
	ret
1:	jmp *%r12		/* child: new stack, rax = 0 */
