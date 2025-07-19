# context switch - the heart of multitasking
#
# context switching is how the kernel implements multitasking:
# - save current process's registers in its context structure
# - load next process's registers from its context structure  
# - return to next process (which continues where it left off)
#
# this happens when:
# - timer interrupt forces a process to yield (preemptive multitasking)
# - process voluntarily yields (calls sleep, exit, etc.)
# - process blocks waiting for i/o
#
# risc-v calling convention:
# - caller-saved registers (t0-t6, a0-a7): caller must save before function call
# - callee-saved registers (s0-s11, ra, sp): function must preserve these
# - we only need to save callee-saved registers since context switch is a function call

#   void swtch(struct context *old, struct context *new);
# 
# save current registers in old, load from new

.globl swtch
swtch:
        # save current context to old (first parameter, in a0 register)
        # store each callee-saved register at its offset in the context structure
        sd ra, 0(a0)    # return address - where to resume execution
        sd sp, 8(a0)    # stack pointer - current stack location
        sd s0, 16(a0)   # saved registers s0-s11
        sd s1, 24(a0)   # these must be preserved across function calls
        sd s2, 32(a0)   # so we save them during context switch
        sd s3, 40(a0)
        sd s4, 48(a0)
        sd s5, 56(a0)
        sd s6, 64(a0)
        sd s7, 72(a0)
        sd s8, 80(a0)
        sd s9, 88(a0)
        sd s10, 96(a0)
        sd s11, 104(a0)

        # load new context from new (second parameter, in a1 register)
        # load each register from the new context structure
        ld ra, 0(a1)    # load return address of new process
        ld sp, 8(a1)    # switch to new process's stack
        ld s0, 16(a1)   # restore saved registers s0-s11
        ld s1, 24(a1)   # these contain the new process's state
        ld s2, 32(a1)
        ld s3, 40(a1)
        ld s4, 48(a1)
        ld s5, 56(a1)
        ld s6, 64(a1)
        ld s7, 72(a1)
        ld s8, 80(a1)
        ld s9, 88(a1)
        ld s10, 96(a1)
        ld s11, 104(a1)
        
        # return to new process
        # ret jumps to address in ra register (just loaded from new context)
        # this resumes execution wherever the new process was when it last yielded
        ret

	
