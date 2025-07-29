// trap handling - managing interrupts, exceptions, and system calls
//
// traps are events that cause the cpu to stop normal execution and jump to kernel code:
// 1. interrupts - external events (timer, device i/o completion)
// 2. exceptions - program errors (page fault, illegal instruction) 
// 3. system calls - user program requests kernel services (ecall instruction)
//
// when a trap occurs:
// 1. hardware saves user registers in trapframe
// 2. hardware switches to supervisor mode and jumps to trap handler
// 3. kernel processes the trap (system call, device interrupt, etc.)
// 4. kernel restores user registers and returns to user mode
//
// this is how the kernel maintains control over the system while allowing
// user programs to run - any attempt to access privileged resources triggers a trap

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;  // synchronizes access to timer tick counter across cpus
uint ticks;       // global counter of timer interrupts since system started

extern char trampoline[], uservec[], userret[];  // assembly code in trampoline.S

// in kernelvec.S, calls kerneltrap()
void kernelvec();

extern int devintr();  // handle device interrupts

// initialize the trap handling system for the entire kernel
// called once during boot to set up global trap-related data structures
// x
void initialize_trap_system_globals(void)
{
  // initialize the lock that protects the global timer tick counter
  // this lock prevents race conditions when multiple cpus access timer_ticks_since_boot
  create_lock(&tickslock, "time");
}

// configure this cpu core to handle traps and exceptions while running kernel code
// called on each cpu during initialization to set up per-cpu trap handling
// this is separate from usertrap handling - kernel and user traps use different vectors
// x
void install_kernel_trap_vector_on_cpu(void)
{
  // set supervisor trap vector register (stvec) to point to kernel trap handler
  // when a trap occurs while cpu is in supervisor mode, hardware jumps to kernelvec
  // kernelvec is defined in kernelvec.S and handles kernel-mode interrupts/exceptions
  w_stvec((uint64)kernelvec);
}

// handle an interrupt, exception, or system call from user space
// this is the main kernel entry point when user programs trigger traps
// called from trampoline.S after saving user registers in trapframe
void usertrap(void)
{
  int device_interrupt_type = 0;  // identifies which device caused interrupt (0=none, 1=uart, 2=timer)

  // security check: verify we actually came from user mode
  // sstatus.spp (supervisor previous privilege) should be 0 for user mode
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // redirect future traps to kernel handler since we're now in supervisor mode
  // if another trap occurs while we're handling this one, it should go to kernelvec
  // this prevents infinite recursion and ensures proper nested trap handling
  w_stvec((uint64)kernelvec);

  struct proc *current_process = myproc();
  
  // preserve user program counter for eventual return to user space
  // sepc (supervisor exception program counter) contains user pc at trap time
  // we save this in trapframe so we can restore it when returning to user
  current_process->trapframe->epc = r_sepc();
  
  // decode trap cause by examining scause register (supervisor cause register)
  // scause contains a code indicating what type of trap occurred
  if(r_scause() == 8){
    // system call trap (ecall instruction executed in user mode)
    // ecall is the mechanism user programs use to request kernel services

    if(killed(current_process))
      exit(-1);  // process was marked for termination, abort system call

    // advance user program counter past the ecall instruction
    // sepc currently points to the ecall, but we want to return to next instruction
    // risc-v instructions are 4 bytes, so add 4 to skip over the ecall
    current_process->trapframe->epc += 4;

    // re-enable interrupts now that we've finished reading trap-sensitive registers
    // during trap entry, interrupts are automatically disabled to ensure atomicity
    // an interrupt could modify sepc, scause, and sstatus if we don't read them first
    intr_on();

    // dispatch to appropriate system call handler based on call number
    // system call number is passed in a7 register (saved in trapframe by trampoline.S)
    syscall();
  } else if((device_interrupt_type = devintr()) != 0){
    // external device interrupt (timer, disk, uart, network, etc.)
    // devintr() examines interrupt controller and handles the specific device
    // returns device type: 1=uart, 2=timer, others for additional devices
  } else {
    // unexpected or unhandled trap - this indicates a serious problem
    // could be: illegal instruction, page fault, alignment error, etc.
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), current_process->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(current_process);  // mark process for termination
  }

  // check if process was killed during trap processing
  // a signal or kill system call might have marked this process for death
  if(killed(current_process))
    exit(-1);

  // implement preemptive scheduling via timer interrupts
  // if this was a timer interrupt (device_interrupt_type == 2), yield cpu to next process
  // this ensures no process can monopolize the cpu indefinitely
  if(device_interrupt_type == 2)
    yield();

  // prepare for return to user space
  // this sets up registers and switches back to user mode
  usertrapret();
}

// transition from kernel mode back to user space
// this function carefully orchestrates the return to user mode after handling a trap
// must restore user state, switch page tables, and change privilege levels atomically
void usertrapret(void)
{
  struct proc *current_process = myproc();

  // disable interrupts during the critical transition period
  // we're about to change trap vector destination from kerneltrap() to usertrap()
  // interrupts must be off until we're safely back in user space
  intr_off();

  // configure trap vector to handle future user-space traps
  // calculate absolute address of uservec within the trampoline page
  // trampoline page is mapped at same virtual address in all page tables
  uint64 user_trap_vector_address = TRAMPOLINE + (uservec - trampoline);
  w_stvec(user_trap_vector_address);  // redirect future traps to user handler

  // prepare trapframe with kernel information needed for next trap
  // when user space traps again, uservec will need this data to enter kernel
  current_process->trapframe->kernel_satp = r_satp();         // current kernel page table
  current_process->trapframe->kernel_sp = current_process->kstack + PGSIZE; // top of kernel stack
  current_process->trapframe->kernel_trap = (uint64)usertrap; // kernel trap handler address
  current_process->trapframe->kernel_hartid = r_tp();         // current cpu core id

  // configure supervisor status register for return to user mode
  // sstatus controls privilege level and interrupt state after sret instruction
  unsigned long supervisor_status_register = r_sstatus();
  supervisor_status_register &= ~SSTATUS_SPP; // clear spp bit = return to user mode (privilege 0)
  supervisor_status_register |= SSTATUS_SPIE; // set spie bit = enable interrupts in user mode
  w_sstatus(supervisor_status_register);

  // set program counter for return to user space
  // sepc will be loaded into pc when sret executes
  w_sepc(current_process->trapframe->epc);

  // create satp register value for user process page table
  // MAKE_SATP converts process page table physical address to satp format
  // trampoline code will load this to switch from kernel to user virtual memory
  uint64 user_page_table_satp = MAKE_SATP(current_process->pagetable);

  // execute the actual return to user space via trampoline code
  // trampoline.S:userret switches page tables, restores registers, executes sret
  // userret function is position-independent and works from any page table
  uint64 trampoline_userret_address = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret_address)(user_page_table_satp);
}

// handle interrupts and exceptions that occur while running kernel code
// called via kernelvec.S when cpu is already in supervisor mode
// uses current kernel stack (no stack switching needed)
void 
kerneltrap()
{
  int device_interrupt_type = 0;  // type of device that caused interrupt
  uint64 saved_exception_program_counter = r_sepc();    // save sepc before potential modification
  uint64 saved_supervisor_status = r_sstatus();         // save sstatus before potential modification  
  uint64 supervisor_cause_register = r_scause();        // trap cause code
  
  // verify we were actually in supervisor mode when trap occurred
  // sstatus.spp should be 1 for supervisor mode
  if((saved_supervisor_status & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
    
  // verify interrupts were disabled when we entered (they should be)
  // kernel code expects interrupts to be off during critical sections
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  // attempt to handle this as a device interrupt
  // if devintr returns 0, this is an unexpected trap/exception
  if((device_interrupt_type = devintr()) == 0){
    // unknown interrupt source or exception while in kernel
    // this indicates a serious kernel bug (null pointer, bad memory access, etc.)
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", supervisor_cause_register, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // implement preemptive scheduling for kernel code
  // if this was a timer interrupt and we have a current process, yield cpu
  // allows other processes to run even if kernel is in a long-running operation
  if(device_interrupt_type == 2 && myproc() != 0)
    yield();

  // restore trap registers to their original values
  // yield() may have caused context switches and other traps to occur
  // kernelvec.S's sret instruction needs original sepc and sstatus values
  w_sepc(saved_exception_program_counter);
  w_sstatus(saved_supervisor_status);
}

// handle timer/clock interrupts for timekeeping and scheduling
// called by device interrupt handler when timer fires
// maintains global tick counter and wakes up sleeping processes
void
clockintr()
{
  // only cpu 0 maintains the global timer tick counter to avoid races
  // other cpus can handle timer interrupts but don't update global state
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;  // increment global time counter
    wake_up(&ticks);  // wake processes sleeping on timer
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

