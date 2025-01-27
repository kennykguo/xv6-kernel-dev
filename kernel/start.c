#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
// This code is running for every CPU! All 8 cores run this at the same time
// Different by accessing the core id / hart id
// Direct variable
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];


// Get into the supervisor level of privilege


// entry.S jumps here in machine mode on stack0.
void start() {
  // The previous privilege level where we came from, is supervisor mode
  // set M Previous Privilege mode to Supervisor, for mret.
  // Set the bit representing privilege to supervisior mode
  unsigned long x = r_mstatus(); // Defined in riscv.h, executes an assembly instruction
  // Clear the bits of machine privilege
  x &= ~MSTATUS_MPP_MASK;
  // Set the bits to be supervisor privilege
  x |= MSTATUS_MPP_S;
  // Write back to the status register
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  // Machine exception program counter
  // Setting the previous instruction to main
  w_mepc((uint64)main);

  // disable paging for now.
  // Address translation - writes a zero turns off virtual memory
  // Every virtual address is still directly mapped into physicial memory
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // Interrupt and exceptions
  // Interrupts - done by the kernel
  // Exceptions - done by user programs
  // Collectively called traps
  w_medeleg(0xffff); // All traps happen in supervisor mode
  w_mideleg(0xffff); // Delegation register for machine mode exceptions
  // Writing 0xffff, all refer to an exception that can happen
  // All traps should be delegated
  
  // Supervisor interrupt enable register
  // Supervisor exception external interrupts, supervisior timer interrupts, supervisor software interrupts
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.

  // Allow supervisor mode to have full access to memory
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // ask for clock interrupts.
  // Initialize the clock interrupt
  // Clock allows for context switches
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // Written to the thread pointer
  // The hardware thread id is a machine mode register
  // Supervisor mode always knows which core some sort of code is running on
  int id = r_mhartid();
  w_tp(id);

  // switch to supervisor mode and jump to main().
  // Machine mode return
  // Return from one privilege level, into another
  // Looks at previous privilege and mepc
  // Run at main, set privilege to mstatus
  asm volatile("mret");
}


// Ask each hart to generate timer interrupts.
// Use a counter, about 1/10 a second

void timerinit()
{
  // enable supervisor-mode timer interrupts.
  w_mie(r_mie() | MIE_STIE);
  
  // enable the sstc extension (i.e. stimecmp).
  w_menvcfg(r_menvcfg() | (1L << 63)); 
  
  // allow supervisor to use stimecmp and time.
  w_mcounteren(r_mcounteren() | 2);
  
  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
