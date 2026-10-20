/* Linux Image entry; XNU payload is embedded after the loader stack. */
.section .text.head, "ax"
.global _head
_head:
 b _start
 .long 0
 .quad 0
 .quad _image_end
 .quad 0xa
 .quad 0, 0, 0
 .ascii "ARM\x64"
 .long 0
.text
_start:
 msr daifset, #0xf
 mov x19, x0
 mrs x1, CurrentEL
 cmp x1, #4
 b.ne fatal
 msr spsel, #1
 adr x1, _stack_top
 mov sp, x1
 adr x1, vectors
 msr vbar_el1, x1
 isb
 adr x1, _bss_start
 adr x2, _bss_end
1:
 cmp x1, x2
 b.hs 2f
 str xzr, [x1], #8
 b 1b
2:
 mov x0, x19
 bl loader_main
fatal:
 mov x0, #9
 movk x0, #0x8400, lsl #16
 smc #0
3:
 wfe
 b 3b
.balign 0x800
vectors:
.rept 4
 b fatal
 .balign 0x80
.endr
 b probe_sync_fault
 .balign 0x80
.rept 11
 b fatal
 .balign 0x80
.endr

/* A synchronous abort inside probe_safe_rd32 resumes at its fixup and
 * counts the fault; any other exception still resets the SoC. */
probe_sync_fault:
 stp x0, x1, [sp, #-16]!
 adrp x0, probe_fault_fixup
 ldr x1, [x0, :lo12:probe_fault_fixup]
 cbz x1, 1f
 msr elr_el1, x1
 str xzr, [x0, :lo12:probe_fault_fixup]
 adrp x0, probe_fault_esr
 mrs x1, esr_el1
 str w1, [x0, :lo12:probe_fault_esr]
 adrp x0, probe_fault_far
 mrs x1, far_el1
 str x1, [x0, :lo12:probe_fault_far]
 ldp x0, x1, [sp], #16
 eret
1:
 ldp x0, x1, [sp], #16
 b fatal

/* uint32_t probe_safe_rd32(uint64_t pa, uint32_t *faulted) */
.global probe_safe_rd32
probe_safe_rd32:
 adrp x2, probe_fault_fixup
 adr x3, 4f
 str x3, [x2, :lo12:probe_fault_fixup]
 dsb sy
 ldr w0, [x0]
 dsb sy
 str xzr, [x2, :lo12:probe_fault_fixup]
 str wzr, [x1]
 ret
4:
 mov w3, #1
 str w3, [x1]
 mov w0, #0xfa17
 movk w0, #0xdead, lsl #16
 ret

.bss
.balign 8
.global probe_fault_esr, probe_fault_far
probe_fault_fixup: .quad 0
probe_fault_far: .quad 0
probe_fault_esr: .long 0, 0
.text

/* Optional isolated diagnostic CPU, never handed to XNU's scheduler. */
.global probe_secondary_entry
probe_secondary_entry:
 msr daifset, #0xf
 msr spsel, #1
 adr x1, _secondary_stack_top
 mov sp, x1
 adr x1, vectors
 msr vbar_el1, x1
 isb
 bl probe_secondary_main
 b fatal
