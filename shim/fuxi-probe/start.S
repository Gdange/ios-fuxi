/* Linux arm64 Image entry. Payload-free: no UFS or framebuffer access. */
.section .text.head, "ax"
.global _head
_head:
 b _start
 .long 0
 .quad 0
 .quad _end
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
 bl probe_main
fatal:
 mov x0, #9
 movk x0, #0x8400, lsl #16
 smc #0
3:
 wfe
 b 3b
.balign 0x800
vectors:
.rept 16
 b fatal
 .balign 0x80
.endr

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
