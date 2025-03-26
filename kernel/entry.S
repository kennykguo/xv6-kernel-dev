# qemu -kernel loads the kernel at 0x80000000 and causes each hart (i.e. CPU) to jump there at the beginning.
# Specifically, kernel.ld causes the following code to be placed at 0x80000000.
# When the computer powers on, the BIOS firmware stored in ROM is the first code to execute
# The BIOS performs a POST (Power-On Self Test) to check hardware
# Then it looks for a bootable device according to the boot priority sequence


.section .text
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + (hartid * 4096)
        # Technically is sp = stack0 + (hartid + 1 * 4096)
        # This is so that we can have the stack grow DOWN by adding 1, reserving space properly

        # Loading the stack pointer
        la sp, stack0

        # Load in 4096
        li a0, 1024*4
        

        # Control status register stores the hart id
        # CSSR atomically reads the value of a control status register and stores it in a general purpose register
        # Guessing mhartid is zero indexed
        csrr a1, mhartid

        # Add 1 to the value
        addi a1, a1, 1

        # Multiply 4086 by the hart id
        mul a0, a0, a1

        # Add to stack pointer
        add sp, sp, a0

        # jump to start() in start.c
        # Jump to C label
        call start

spin: # We won't ever end up here, just as a safety net. WE should never return from start
        j spin
