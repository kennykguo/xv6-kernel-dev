#include "types.h"  // defines basic types used throughout the kernel (uint64, etc.)
#include "param.h"  // contains kernel parameters like NCPU (number of CPUs)
#include "memlayout.h" // defines the memory layout of the system
#include "riscv.h"  // contains RISC-V specific functions and definitions
#include "defs.h"   // function declarations used throughout the kernel

// forward declarations
void main();
void timerinit();

// risc-v privilege levels from highest to lowest:
// machine mode (M) - highest privilege, can access all hardware
// supervisor mode (S) - OS kernel runs here, limited hardware access  
// user mode (U) - user programs run here, most restricted

// when cpu boots, it starts in machine mode
// we need to set up the environment and drop to supervisor mode to run the kernel
// this allows proper isolation between kernel and user programs

// entry.S needs one stack per CPU core
// this array provides 4096 bytes of stack space per cpu
// aligned to 16 bytes as required by risc-v calling convention



// gcc compiler attribute that forces memory alignment to 16-byte boundaries
// this ensures the stack0 array starts at a memory address divisible by 16
__attribute__ ((aligned (16))) 
// declares an array of char (1-byte) elements
char stack0 [4096 * NCPU];
// array size: 4096 bytes per cpu multiplied by NCPU (number of cpus)
// creates a contiguous block of memory for all cpu stacks
// this array gets placed in the .bss section (uninitialized global data)


// entry.S jumps here while running in machine mode after setting up stack, on all cpus
// this function configures the risc-v privilege system and jumps to supervisor mode
// supervisor mode is where the kernel should run for proper isolation
// https://github.com/riscv-software-src/opensbi
void start() {
  
    // configure where to return when we execute mret (machine return)
    // mret will jump to supervisor mode and execute the address in mepc
    
    // first, set up mstatus register to control privilege transition
    unsigned long x = r_mstatus(); // read current machine status register
    
    // mstatus.MPP (machine previous privilege) controls what privilege level
    // we'll be in after executing mret instruction
    // clear the MPP field (bits 11-12) first
    x &= ~MSTATUS_MPP_MASK;
    
    // set MPP to supervisor mode so mret will jump to supervisor mode
    x |= MSTATUS_MPP_S;
    
    // write back the modified status register
    w_mstatus(x);

    // set mepc (machine exception program counter) to main function
    // when we execute mret, cpu will jump to this address
    // the medany code model is required because it allows position-independent code
    w_mepc((uint64)main);

    // disable paging initially (virtualization)
    // we'll enable paging later after setting up page tables
    // satp (supervisor address translation and protection) controls virtual memory
    // setting it to 0 disables virtual memory - all addresses are physical
    // explicitly ensures physical addressing
    w_satp(0);

    // delegate interrupts and exceptions to supervisor mode
    // by default, all traps go to machine mode
    // but we want the kernel (running in supervisor mode) to handle them

    // medeleg = machine exception delegation register (csr 0x302)
    // controls which exceptions get delegated from machine mode to supervisor mode
    // each bit corresponds to a specific exception cause number
    w_medeleg(0xffff); 

    // mideleg = machine interrupt delegation register (csr 0x303) 
    // controls which interrupts get delegated from machine mode to supervisor mode
    // each bit corresponds to a specific interrupt cause number
    w_mideleg(0xffff);
    
    // enable interrupts in supervisor mode
    // sie (supervisor interrupt enable) controls which interrupt types are enabled
    // enables external, software, timer interrupts
    // SEIE = supervisor external interrupt enable (devices) bit 
    // STIE = supervisor timer interrupt enable (for preemptive scheduling) bit
    // SSIE = supervisor software interrupt enable (inter-processor interrupts) bit
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    // configure physical memory protection (PMP)
    // by default, supervisor mode cannot access any physical memory
    // we need to explicitly grant access to all memory
    // pmpaddr0 sets the address range for PMP region 0
    // 0x3fffffffffffffull covers all possible physical addresses
    // stored value: 0x3fffffffffffff  
    // actual address boundary: 0x3fffffffffffff << 2 = 0xfffffffffffffffc
    // covers 0x0 to 0xfffffffffffffffc (nearly entire 64-bit address space)
    w_pmpaddr0(0x3fffffffffffffull);
    
    // pmpcfg0 configures permissions for PMP region 0
    // 0xf = read + write + execute permissions for supervisor mode
    w_pmpcfg0(0xf);

    // set up timer interrupts for preemptive multitasking
    // without timer interrupts, processes could run forever without yielding
    timerinit();

    // store this cpu's id in thread pointer register for easy access
    // the kernel frequently needs to know which cpu is running
    // tp register is accessible from supervisor mode and perfect for this
    int id = r_mhartid();
    w_tp(id);

    // mret (machine return) instruction:
    // 1. sets privilege level to value in mstatus.MPP (supervisor mode)
    // 2. jumps to address in mepc (main function)
    // 3. enables interrupts if mstatus.MPIE is set
    asm volatile("mret");
    
}

// configure timer interrupts for preemptive scheduling
// timer interrupts are crucial for multitasking - they force running processes
// to yield cpu periodically so other processes can run
void timerinit()
{
    // enable supervisor-mode timer interrupts in machine interrupt enable register
    w_mie(r_mie() | MIE_STIE);
    
    // enable sstc extension (supervisor timer compare)
    // this allows supervisor mode to directly program timer interrupts
    w_menvcfg(r_menvcfg() | (1L << 63)); 
    
    // allow supervisor mode to access time counter and stimecmp
    // mcounteren controls which counters supervisor mode can access
    w_mcounteren(r_mcounteren() | 2);
    
    // schedule the first timer interrupt
    // stimecmp (supervisor timer compare) triggers interrupt when time >= stimecmp
    // 1000000 cycles is roughly 1/10 second on typical risc-v systems
    w_stimecmp(r_time() + 1000000);
}
