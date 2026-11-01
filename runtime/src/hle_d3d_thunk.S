/* Entry glue for HLE'd D3D8 functions (src/hle_d3d.c). i386, AT&T syntax. Same frame as
 * hle_entry_common (hle_thunk.S): each patched guest function jumps to a per-entry stub
 *   push $index; call hle_d3d_entry_common; add $4,%esp; ret $argbytes
 * so on entry: (%esp)=return into stub, 4(%esp)=index, 8(%esp)=guest caller, 12(%esp)=args.
 * pushal saves ecx/edx too, so register-argument entries (SetRenderState_Simple) see them. */
#ifdef _WIN32
#define SYM(x) _##x   /* i386 COFF: C symbols carry a leading underscore */
#else
#define SYM(x) x
#endif
    .text
    .globl SYM(hle_d3d_entry_common)
SYM(hle_d3d_entry_common):
    pushal
    cld
    movl %esp, %ebp          /* D3dFrame* */
    andl $-16, %esp
    subl $12, %esp
    pushl %ebp
    call SYM(hle_d3d_dispatch)
    movl %ebp, %esp
    popal                    /* restores eax written by the dispatcher */
    ret

#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
