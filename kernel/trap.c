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

struct spinlock tickslock;  // protects ticks counter
uint ticks;                 // number of timer interrupts since boot

extern char trampoline[], uservec[], userret[];  // assembly code in trampoline.S

// in kernelvec.S, calls kerneltrap()
void kernelvec();

extern int devintr();  // handle device interrupts

// initialize trap handling system
void trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel
// install kernel trap vector so that traps while in kernel mode go to kernelvec
void trapinithart(void)
{
  w_stvec((uint64)kernelvec);  // set supervisor trap vector to kernelvec function
}

// handle an interrupt, exception, or system call from user space
// called from trampoline.S after saving user registers
void usertrap(void)
{
  int which_dev = 0;

  // verify we came from user mode (should always be true)
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap() since we're now in the kernel
  // if another trap occurs while we're handling this one, it should go to kernel handler
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter in trapframe
  // sepc contains the user pc at the time of the trap
  p->trapframe->epc = r_sepc();
  
  // determine what kind of trap this is by examining scause register
  if(r_scause() == 8){
    // system call (ecall from user mode)

    if(killed(p))
      exit(-1);  // process was killed, don't execute system call

    // sepc points to the ecall instruction, but we want to return to the next instruction
    // increment saved pc so user program continues after the ecall
    p->trapframe->epc += 4;

    // enable interrupts now that we're done reading trap-sensitive registers
    // an interrupt could change sepc, scause, and sstatus
    intr_on();

    // dispatch the system call based on number in a7 register
    syscall();
  } else if((which_dev = devintr()) != 0){
    // device interrupt (timer, disk, uart, etc.)
    // devintr() handles the specific device and returns device type
  } else {
    // unexpected trap - print error and kill process
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  // check if process was killed during trap handling
  if(killed(p))
    exit(-1);

  // give up the CPU if this was a timer interrupt (preemptive scheduling)
  // timer interrupts force processes to yield periodically for fairness
  if(which_dev == 2)
    yield();

  // return to user space
  usertrapret();
}

// return to user space
// this function prepares for the transition back to user mode
void usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from kerneltrap() to usertrap()
  // so turn off interrupts until we're back in user space where usertrap() is correct
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  // calculate address of uservec within the trampoline page
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);  // set trap vector to user trap handler

  // set up trapframe values that uservec will need when next user trap occurs
  p->trapframe->kernel_satp = r_satp();         // kernel page table for next trap
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // kernel stack pointer
  p->trapframe->kernel_trap = (uint64)usertrap; // address of usertrap function
  p->trapframe->kernel_hartid = r_tp();         // this cpu's id

  // set up the registers that trampoline.S's sret will use to get to user space
  
  // set S Previous Privilege mode to User
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
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

