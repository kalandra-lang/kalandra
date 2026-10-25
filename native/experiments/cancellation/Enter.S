.text
.globl _kalandra_cancel_enter
.p2align 2
_kalandra_cancel_enter:
  .cfi_startproc
  stp x29, x30, [sp, #-32]!
  .cfi_def_cfa_offset 32
  .cfi_offset w29, -32
  .cfi_offset w30, -24
  str x28, [sp, #16]
  .cfi_offset w28, -16
  mov x29, sp
  mov x28, x2
  mov x16, x0
  mov x0, x1
  blr x16
  ldr x28, [sp, #16]
  ldp x29, x30, [sp], #32
  ret
  .cfi_endproc

// Adversarial ABI test: an ordinary C++ runtime is permitted to borrow x28
// across a callback. The callback adapter must restore the task register.
.globl _kalandra_cancel_borrow_register
.p2align 2
_kalandra_cancel_borrow_register:
  .cfi_startproc
  stp x29, x30, [sp, #-32]!
  .cfi_def_cfa_offset 32
  .cfi_offset w29, -32
  .cfi_offset w30, -24
  str x28, [sp, #16]
  .cfi_offset w28, -16
  mov x29, sp
  mov x16, x0
  mov x0, x1
  mov x28, #0xbad
  blr x16
  cmp x28, #0xbad
  b.ne 1f
  ldr x28, [sp, #16]
  ldp x29, x30, [sp], #32
  ret
1:
  brk #1
  .cfi_endproc
