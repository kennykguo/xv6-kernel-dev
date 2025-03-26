#include "types.h"  // Defines basic types used throughout the kernel (uint64, etc.)
#include "param.h"  // Contains kernel parameters like NCPU (number of CPUs)
#include "memlayout.h" // Defines the memory layout of the system
#include "riscv.h"  // Contains RISC-V specific functions and definitions
#include "defs.h"   // Function declarations used throughout the kernel

// Forward declaration
void main();
void timerinit();

// entry.S needs one stack per CPU.
// This code is running for every CPU! All 8 cores run this at the same time
// Different by accessing the core id / hart id
// Direct variable

// Variable is used in entry.S
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];


// entry.S jumps here in machine mode on stack0.
// This function will set the the supervisor level of privilege to run the actual kernel
void start() {
  // Overall summary - - - 
  // The previous privilege level where we came from, is supervisor mode
  // set M Previous Privilege mode to Supervisor, for mret.
  // Set the bit representing privilege to supervisior mode

  unsigned long x = r_mstatus(); // Defined in riscv.h, executes an assembly instruction
  
  // Clear the bits of machine privilege
  // Clears the MPP (Machine Previous Privilege) bits in the register
  x &= ~MSTATUS_MPP_MASK;

  // Set the bits to be supervisor privilege
  x |= MSTATUS_MPP_S;

  // Write back to the status register
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret (executed later, the CPU will then jump to this address)
  // The medany code model (specified in compiler flags) is required because it allows the code to access all of memory regardless of where it's placed
  // requires gcc -mcmodel=medany
  // Machine exception program counter
  // Setting the previous instruction to main
  w_mepc((uint64)main);

  // Disable paging for now
  // Address translation - writes a zero turns off virtual memory
  // Every virtual address is still directly mapped into physical memory
  w_satp(0);

  // Delegate all interrupts and exceptions to supervisor mode.
  // Interrupt and exceptions
  // Interrupts - done by the kernel
  // Exceptions - done by user programs
  // Collectively called traps
  w_medeleg(0xffff); // Writes to machine exception delegation register - All traps happen in supervisor mode
  w_mideleg(0xffff); // Delegation register for machine mode exceptions - Delegation register for machine mode exceptions
  // Writing 0xffff, all refer to an exception that can happen
  // All traps should be delegated
  

  // Supervisor interrupt enable register - enables specific interrupt types in the OS
  // Supervisor exception external interrupts, supervisior timer interrupts, supervisor software interrupts
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.

  // Allow supervisor mode to have full access to memory
  // Sets the address register for PMP region 0 to cover all of memory
  w_pmpaddr0(0x3fffffffffffffull);

  // Sets the configuration for PMP region 0 to allow full access (read, write, execute) from supervisor mode
  w_pmpcfg0(0xf);

  // ask for clock interrupts.
  // Initialize the clock interrupt
  // Clock allows for context switches
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // Written to the thread pointer
  // The hardware thread id is a machine mode register
  // Supervisor mode always knows which core some sort of code is running on
  // The thread pointer can be accessed from supervisor mode, allowing the kernel to identify which CPU is executing code
  int id = r_mhartid();
  w_tp(id);

  // Switch to supervisor mode and jump to main()
  // Machine mode return
  // Return from one privilege level, into another
  // Looks at previous privilege and mepc
  // Run at main, set privilege to mstatus
  // Volatile to prevent the compiler from optimizing away this critical instruction
  asm volatile("mret");
}


// Ask each hart to generate timer interrupts.
// Use a counter, about 1/10 a second
void timerinit()
{
  // Enable supervisor-mode timer interrupts
  w_mie(r_mie() | MIE_STIE);
  
  // Enable the sstc extension (i.e. stimecmp) - set the highest bit of this register to exmaple stimecmp
  w_menvcfg(r_menvcfg() | (1L << 63)); 
  
  // Allow supervisor to use stimecmp and time - Supervisor mode to access time counter
  w_mcounteren(r_mcounteren() | 2);
  
  // Ask for the very first timer interrupt
  w_stimecmp(r_time() + 1000000);
}
