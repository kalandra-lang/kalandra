#pragma once
// Explicit ABI sentinels around a genuinely interrupted native frame.
extern "C" bool kalandra_test_checkpoint_registers(bool (*)(void *), void *);
extern "C" bool kalandra_test_checkpoint_spin(void *);
asm(R"(
 .text
.globl _kalandra_test_checkpoint_registers
.p2align 2
_kalandra_test_checkpoint_registers:
 .cfi_startproc
 stp x29, x30, [sp, #-160]!
 .cfi_def_cfa_offset 160
 .cfi_offset w29, -160
 .cfi_offset w30, -152
 stp x19, x20, [sp, #16]
 .cfi_offset w19, -144
 .cfi_offset w20, -136
 stp x21, x22, [sp, #32]
 .cfi_offset w21, -128
 .cfi_offset w22, -120
 stp x23, x24, [sp, #48]
 .cfi_offset w23, -112
 .cfi_offset w24, -104
 stp x25, x26, [sp, #64]
 .cfi_offset w25, -96
 .cfi_offset w26, -88
 stp x27, x28, [sp, #80]
 .cfi_offset w27, -80
 .cfi_offset w28, -72
 stp d8, d9, [sp, #96]
 .cfi_offset b8, -64
 .cfi_offset b9, -56
 stp d10, d11, [sp, #112]
 .cfi_offset b10, -48
 .cfi_offset b11, -40
 stp d12, d13, [sp, #128]
 .cfi_offset b12, -32
 .cfi_offset b13, -24
 stp d14, d15, [sp, #144]
 .cfi_offset b14, -16
 .cfi_offset b15, -8
 mov x29, sp
 mov x16, x0
 mov x0, x1
 mov x19, #275
 mov x20, #276
 mov x21, #277
 mov x22, #278
 mov x23, #279
 mov x24, #280
 mov x25, #281
 mov x26, #282
 mov x27, #283
 mov x28, #284
 mov x9, #520
 fmov d8, x9
 mov x9, #521
 fmov d9, x9
 mov x9, #522
 fmov d10, x9
 mov x9, #523
 fmov d11, x9
 mov x9, #524
 fmov d12, x9
 mov x9, #525
 fmov d13, x9
 mov x9, #526
 fmov d14, x9
 mov x9, #527
 fmov d15, x9
 blr x16
 cmp x19, #275
 b.ne 9f
 cmp x20, #276
 b.ne 9f
 cmp x21, #277
 b.ne 9f
 cmp x22, #278
 b.ne 9f
 cmp x23, #279
 b.ne 9f
 cmp x24, #280
 b.ne 9f
 cmp x25, #281
 b.ne 9f
 cmp x26, #282
 b.ne 9f
 cmp x27, #283
 b.ne 9f
 cmp x28, #284
 b.ne 9f
 fmov x9, d8
 cmp x9, #520
 b.ne 9f
 fmov x9, d9
 cmp x9, #521
 b.ne 9f
 fmov x9, d10
 cmp x9, #522
 b.ne 9f
 fmov x9, d11
 cmp x9, #523
 b.ne 9f
 fmov x9, d12
 cmp x9, #524
 b.ne 9f
 fmov x9, d13
 cmp x9, #525
 b.ne 9f
 fmov x9, d14
 cmp x9, #526
 b.ne 9f
 fmov x9, d15
 cmp x9, #527
 b.ne 9f
 ldp x19, x20, [sp, #16]
 ldp x21, x22, [sp, #32]
 ldp x23, x24, [sp, #48]
 ldp x25, x26, [sp, #64]
 ldp x27, x28, [sp, #80]
 ldp d8, d9, [sp, #96]
 ldp d10, d11, [sp, #112]
 ldp d12, d13, [sp, #128]
 ldp d14, d15, [sp, #144]
 ldp x29, x30, [sp]
 add sp, sp, #160
 ret
9:
 brk #7
 .cfi_endproc
 .section __TEXT,__kalandra,regular,pure_instructions
.globl _kalandra_test_checkpoint_spin
.p2align 2
_kalandra_test_checkpoint_spin:
 mov x19, #787
 mov x20, #788
 mov x21, #789
 mov x22, #790
 mov x23, #791
 mov x24, #792
 mov x25, #793
 mov x26, #794
 mov x27, #795
 mov x29, #0xbad
 mov x9, #1032
 fmov d8, x9
 mov x9, #1033
 fmov d9, x9
 mov x9, #1034
 fmov d10, x9
 mov x9, #1035
 fmov d11, x9
 mov x9, #1036
 fmov d12, x9
 mov x9, #1037
 fmov d13, x9
 mov x9, #1038
 fmov d14, x9
 mov x9, #1039
 fmov d15, x9
 mov w9, #1
 stlrb w9, [x0]
1:
 b 1b
 .text
)");
