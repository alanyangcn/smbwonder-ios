// One static stop thunk per Wonder NVN loader name (iOS forbids generating code at run time).
// Thunk k = movz x9,#k; b entry. Entry saves x29/x30 and x0-x7, then stops the guest with the name.
#include "nvn_names.h"
    .text
    .globl _wonder_nvn_unimpl_stubs
    .p2align 3
_wonder_nvn_unimpl_stubs:
    .set k, 0
    .rept WONDER_NVN_NAME_COUNT
    movz x9, #k
    b _wonder_nvn_unimpl_entry
    .set k, k + 1
    .endr

    .p2align 2
_wonder_nvn_unimpl_entry:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    mov x0, x9
    mov x1, sp
    bl _wonder_nvn_unimplemented
    brk #0xf00d
