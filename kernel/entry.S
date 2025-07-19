# the boot sequence from first principles:
# 1. when computer powers on, cpu starts executing code from a fixed address in rom
# 2. the rom contains bios/uefi firmware that performs hardware initialization
# 3. bios/uefi finds bootable storage device and loads bootloader  
# 4. bootloader (or in our case, qemu) loads kernel at 0x80000000
# 5. cpu jumps to this address and starts executing our kernel
# 6. this entry.S file is the very first kernel code that runs

# qemu -kernel loads the kernel at 0x80000000 and causes each hart (i.e. CPU) to jump there at the beginning.
# specifically, kernel.ld causes the following code to be placed at 0x80000000.
# when the computer powers on, the BIOS firmware stored in ROM is the first code to execute
# the BIOS performs a POST (Power-On Self Test) to check hardware
# then it looks for a bootable device according to the boot priority sequence

.section .text
.global _entry
_entry:
        # fundamental problem: c code needs a stack to function
        # the stack stores local variables, function parameters, return addresses
        # without a stack, we cannot call c functions or have local variables
        # so first job is to set up a per-cpu stack before jumping to c code
        
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU core
        # sp = stack0 + (hartid * 4096) -> hartid is a CPU core
        # technically is sp = stack0 + (hartid + 1 * 4096)
        # this is so that we can have the stack grow DOWN by adding 1, reserving space properly

        # stacks in risc-v grow downward (from high addresses to low addresses)
        # so we need to point sp to the HIGH end of each cpu's stack space
        # each cpu needs its own stack to avoid corruption when multiple cpus run simultaneously

        # load address of stack0 array into stack pointer
        la sp, stack0

        # each cpu gets 4096 bytes of stack space
        li a0, 1024*4

        # get this cpu's hardware thread id (hart id)
        # mhartid is a machine-mode csr (control status register)
        # csrr atomically reads the csr value into a general purpose register
        csrr a1, mhartid

        # add 1 to hart id so stack calculation works correctly
        # this ensures cpu 0 gets stack0[4096..8191], cpu 1 gets stack0[8192..12287], etc.
        addi a1, a1, 1

        # multiply stack size by (hartid + 1) to get offset for this cpu
        mul a0, a0, a1

        # set stack pointer to this cpu's stack area
        # now sp points to the top of this cpu's dedicated stack
        # all cores run this part of the code, and have different stack pointers
        add sp, sp, a0

        # jump to start() function in start.c
        # now we can safely call c functions since stack is set up
        call start



spin:
        # safety net - start() should never return
        # when somehow start() if it does at all returns, just spin forever
        # this prevents cpu from executing random memory as instructions
        j spin
