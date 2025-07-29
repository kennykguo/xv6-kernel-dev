#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// synchronization flag to ensure cpu 0 completes initialization before other cpus start
// volatile prevents compiler optimization that might cache this value
volatile static int started = 0;

// main kernel initialization function
// called from start() after transitioning to supervisor mode
// this function initializes all major kernel subsystems in dependency order

void main() {
    // only cpu 0 does the main initialization to avoid race conditions
    // other cpus wait until initialization is complete, then do minimal setup
    if(cpuid() == 0){

        // initialize console first so we can print debug/status messages
        // must come early since other init functions may want to print messages
        console_init();
        printf_init();
        printf("\n");
        printf("xv6 kernel is booting\n");
        printf("hello kenny!\n");
        printf("\n");
        
        // memory management initialization - must come early since everything needs memory
        // kernel_init_memory_allocator() sets up the physical page allocator
        // physical memory is divided into 4KB pages, and creates a free list
        kernel_init_memory_allocator();         
        
        // virtual memory initialization
        // initialize_kernel_virtual_memory() creates the kernel's page table mapping virtual addresses to physical
        // the kernel runs in virtual memory for protection and convenience
        initialize_kernel_virtual_memory();  

        // enable virtual memory (paging) on this cpu
        // loads the kernel page table into satp register and turns on address translation
        enable_kernel_virtual_memory_on_cpu();

        // process management initialization
        // initializes the process table, locks, and sets up initial process structures
        initialize_process_table();      
        
        // trap/interrupt handling initialization
        // initializes global trap system locks and data structures
        initialize_trap_system_globals();      
        
        // install kernel trap vector for this cpu
        // loads the kernel trap handler address into the stvec register
        install_kernel_trap_vector_on_cpu();  
        
        // interrupt controller initialization
        // configures global interrupt priorities for all devices in the PLIC
        // the PLIC routes external device interrupts to cpus based on priority
        configure_global_interrupt_priorities();      
        
        // enable device interrupts for this cpu
        // configures PLIC to send UART and disk interrupts to this specific cpu
        enable_interrupts_for_this_cpu();  
        
        // file system initialization begins here
        // these must be done in order due to dependencies
        
        // buffer cache initialization
        // binit() sets up the buffer cache for caching disk blocks in memory
        // the buffer cache improves performance by avoiding redundant disk reads
        binit();         
        
        // inode table initialization  
        // iinit() reads the superblock and initializes the in-memory inode table
        // inodes contain metadata about files and directories
        iinit();         
        
        // file table initialization
        // fileinit() initializes the system-wide file table
        // this tracks all open files across all processes
        fileinit();      
        
        // storage device initialization
        // virtio_disk_init() initializes the emulated virtio disk device
        // this provides the persistent storage for the file system
        virtio_disk_init(); 
        
        // create the first user process
        // userinit() creates the init process that will spawn the shell
        // this is process id 1, the ancestor of all user processes
        userinit();      
        
        // memory barrier to ensure all initialization is complete before setting started flag
        __sync_synchronize();
        started = 1;
    } 
    else {
        // other cpus (hart 1, 2, 3, etc.) wait for cpu 0 to finish initialization
        // this prevents race conditions during bootup
        while(started == 0)
            ;
        
        // memory barrier to ensure we see all the initialization done by cpu 0
        __sync_synchronize();
        printf("hart %d starting\n", cpuid());
        
        // each additional cpu needs to:
        // enable virtual memory for itself
        enable_kernel_virtual_memory_on_cpu();    
        
        // install trap vector for itself  
        install_kernel_trap_vector_on_cpu();   
        
        // enable device interrupts for itself
        enable_interrupts_for_this_cpu();   
    }

    // all cpus end up here running the scheduler
    // the scheduler is an infinite loop that:
    // 1. finds a runnable process
    // 2. switches to that process
    // 3. when process yields/exits, switches back and repeats
    // this function never returns - cpus spend their entire life in the scheduler
    scheduler();        
}
