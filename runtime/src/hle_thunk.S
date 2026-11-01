/* Entry glue for HLE'd guest functions (src/hle_audio.c). i386, AT&T syntax.
 * Each patched guest function jumps to a per-entry host stub:
 *   push $index; call hle_entry_common; add $4,%esp; ret $argbytes
 * so on entry: (%esp)=return into stub, 4(%esp)=index, 8(%esp)=guest caller, 12(%esp)=args. */
#ifdef _WIN32
#define SYM(x) _##x   /* i386 COFF: C symbols carry a leading underscore */
#else
#define SYM(x) x
#endif
    .text
    .globl SYM(hle_entry_common)
SYM(hle_entry_common):
    pushal
    cld
    movl %esp, %ebp          /* HleFrame* */
    andl $-16, %esp
    subl $12, %esp
    pushl %ebp
    call SYM(hle_audio_dispatch)
    movl %ebp, %esp
    popal                    /* restores eax written by the dispatcher */
    ret

#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
