/* Assembly glue between guest (Xbox) code and the host runtime. i386, AT&T syntax. */
#ifdef _WIN32
#define SYM(x) _##x   /* i386 COFF: C symbols carry a leading underscore */
#else
#define SYM(x) x
#endif
    .text

/* Common kernel entry. Each per-ordinal stub does:
 *   push $ordinal; call kentry_common; add $4,%esp; ret $argbytes
 * so on entry: (%esp)=return into stub, 4(%esp)=ordinal, 8(%esp)=guest caller. */
    .globl SYM(kentry_common)
SYM(kentry_common):
    pushal
    cld
    movl %esp, %ebp          /* KFrame* */
    andl $-16, %esp
    subl $12, %esp
    pushl %ebp
    call SYM(kernel_dispatch)
    movl %ebp, %esp
    popal                    /* restores eax/edx written by the dispatcher */
    ret

/* uint32_t guest_call(uint32_t fn, uint32_t stack_top, int nargs, const uint32_t *args)
 * Switches to stack_top, pushes args (stdcall order) and calls fn; works for stdcall
 * and cdecl because esp is restored from ebp afterwards. Returns eax. */
    .globl SYM(guest_call)
SYM(guest_call):
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, %ebp
    movl 20(%ebp), %eax
    movl 24(%ebp), %ebx
    movl 28(%ebp), %ecx
    movl 32(%ebp), %esi
    testl %ebx, %ebx
    jz 1f
    movl %ebx, %esp
1:  testl %ecx, %ecx
    jz 2f
    decl %ecx
    pushl (%esi,%ecx,4)
    jmp 1b
2:  call *%eax
    movl %ebp, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

/* Reached from a per-hook stub (push $index; jmp d3d_hook_entry) patched over the
 * hooked function: (%esp) = hook index, 4(%esp) = caller of the hooked function. */
    .globl SYM(d3d_hook_entry)
SYM(d3d_hook_entry):
    movl (%esp), %eax
    movl 4(%esp), %ecx
    andl $-16, %esp
    subl $8, %esp
    pushl %ecx
    pushl %eax
    call SYM(d3d_hook_reached)     /* noreturn */
    hlt

#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
