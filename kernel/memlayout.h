// physical memory layout - understanding the risc-v memory map
//
// in risc-v systems, physical memory is mapped to specific address ranges
// different hardware devices appear at fixed physical addresses
// this is called "memory-mapped i/o" - devices look like memory to the cpu

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
// qemu simulates a risc-v computer and places devices at these addresses:
//
// 00001000 -- boot ROM, provided by qemu 
//             this contains the first code that runs when machine powers on
//             the boot rom jumps to 0x80000000 where our kernel starts
//
// 02000000 -- CLINT (core local interrupter)
//             handles timer interrupts and inter-processor interrupts
//             each cpu core can send interrupts to other cores through this
//
// 0C000000 -- PLIC (platform-level interrupt controller)
//             routes external device interrupts to cpu cores
//             devices like uart and disk send interrupts through the PLIC
//
// 10000000 -- uart0 (universal asynchronous receiver/transmitter)
//             serial communication device for console input/output
//             reading/writing these addresses sends/receives characters
//
// 10001000 -- virtio disk 
//             emulated hard disk drive for persistent storage
//             the file system reads/writes disk blocks through this interface
//
// 80000000 -- start of RAM (random access memory)
//             this is where qemu loads our kernel
//             after 0x80000000 is unused RAM that the kernel can allocate

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
//             kernel code starts here and grows upward
// kernel_binary_end -- start of kernel page allocation area  
//             'kernel_binary_end' symbol marks end of kernel binary
// PHYSTOP   -- end of RAM used by the kernel
//             we limit ourselves to 128MB of RAM

// memory-mapped device registers
// writing to these addresses controls hardware devices

// qemu puts UART registers here in physical memory.
// uart (serial port) for console input/output
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface for emulated disk
// virtio is a standardized interface for virtual devices
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// qemu puts platform-level interrupt controller (PLIC) here.
// PLIC receives interrupts from devices and routes them to cpus
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)        // interrupt priority registers
#define PLIC_PENDING (PLIC + 0x1000)      // pending interrupt bits
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)    // enable bits for supervisor mode
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000) // priority threshold
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)   // claim/complete register

// physical RAM layout
// the kernel expects there to be RAM for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80000000L               // start of kernel in physical memory  
#define PHYSTOP (KERNBASE + 128*1024*1024) // end of physical memory (128MB)

// virtual memory layout constants
// these define where things appear in virtual address space

// map the trampoline page to the highest address,
// in both user and kernel space.
// the trampoline contains assembly code for entering/exiting the kernel
// it must be at the same virtual address in user and kernel page tables
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
// guard pages cause page faults if stack overflows, catching bugs
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// user memory layout (virtual addresses):
// user processes see this virtual memory layout:
// address zero first:
//   text        - executable code
//   original data and bss - global variables  
//   fixed-size stack      - grows downward from high address
//   expandable heap       - grows upward, allocated by malloc
//   ...
//   TRAPFRAME   - saved registers when entering kernel
//   TRAMPOLINE  - code for entering/exiting kernel
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
