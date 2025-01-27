        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + (hartid * 4096)
        # Technically is sp = stack0 + (hartid + 1 * 4096)
        # This is so that we can have the stack grow DOWN by adding 1, reserving space properly

        # Loading the stack pointer (right to left)
        la sp, stack0

        # Load in 4096 (decimal value)
        li a0, 1024*4

        # Control status register stores the hart id
        csrr a1, mhartid

        # Add 1
        addi a1, a1, 1

        # Multiply a0 by the hart id
        mul a0, a0, a1

        # Add to stack pointer
        add sp, sp, a0

        # jump to start() in start.c
        # Jump to C label
        call start

spin: # We won't ever end up here, just as a safety net. WE should never return from start
        j spin
