// physical memory allocator, for user processes,
// kernel stacks, page-table pages, and pipe buffers. 
// allocates whole 4096-byte pages.
//
// memory management fundamental concepts:
// - physical memory is divided into 4096-byte pages
// - kernel maintains a free list of available pages  
// - kalloc() removes a page from free list and returns it
// - kfree() adds a page back to the free list
// - this is a simple but effective memory allocation strategy

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

// 'end' symbol is defined by kernel.ld linker script
// it marks the end of the kernel binary in memory
// all memory after 'end' can be used for dynamic allocation
extern char end[]; 

// free page list node structure - clever space-saving technique!
// instead of separate metadata, we store the free list directly
// in the free pages themselves. since free pages aren't being used,
// we can store a pointer in the first 8 bytes of each free page.
// this eliminates the need for a separate data structure to track free pages
struct free_page_list_node {
  struct free_page_list_node *next_free_page; // pointer to next available page
};

// global physical memory allocator state
struct {
  struct spinlock physical_memory_lock;   // protects free list from concurrent cpu access
  struct free_page_list_node *free_page_list_head;  // head of linked list of available pages
} kernel_memory_allocator;

// initialize the physical memory allocator subsystem
// called once during kernel startup to set up page allocation
void kinit()
{
  // initialize spinlock to protect free page list from race conditions
  // multiple cpu cores may call kalloc/kfree simultaneously during operation
  init_lock(&kernel_memory_allocator.physical_memory_lock, "physical_memory_allocator");
  
  // populate free list with all available physical memory pages
  // memory from end of kernel binary to PHYSTOP (physical memory limit) is available
  // this creates the initial pool of allocatable pages
  freerange(end, (void*)PHYSTOP);
}

// add all pages in range [physical_address_start, physical_address_end) to free list
// called during initialization to populate the initial free page pool
void freerange(void *physical_address_start, void *physical_address_end)
{
  char *current_page_address;
  
  // round up start address to next page boundary
  // ensures we only free complete 4096-byte aligned pages
  current_page_address = (char*)PGROUNDUP((uint64)physical_address_start);
  
  // iterate through each 4kb page in the specified range
  for(; current_page_address + PGSIZE <= (char*)physical_address_end; current_page_address += PGSIZE)
    kfree(current_page_address); // add this page to the free list
}

// return a physical memory page to the free pool
// accepts a page address that was previously allocated by kalloc()
// (exception: during initialization, this is called on never-allocated pages)
void kfree(void *physical_address)
{
  struct free_page_list_node *new_free_page_node;

  // comprehensive sanity checks to catch programming errors:
  // 1. physical_address must be page-aligned (multiple of 4096 bytes)
  // 2. physical_address must not point into kernel code/data section
  // 3. physical_address must be within valid physical memory range
  if(((uint64)physical_address % PGSIZE) != 0 || 
     (char*)physical_address < end || 
     (uint64)physical_address >= PHYSTOP)
    panic("kfree");

  // security feature: fill page with garbage to catch use-after-free bugs
  // if code accidentally tries to use freed memory, it gets junk instead of old data
  // this debugging technique helps catch dangling pointer vulnerabilities
  memset(physical_address, 1, PGSIZE);

  // prepare to add page to front of free list
  // clever trick: treat the freed page itself as a free_page_list_node structure
  new_free_page_node = (struct free_page_list_node*)physical_address;

  // critical section: update free list atomically to prevent race conditions
  acquire(&kernel_memory_allocator.physical_memory_lock);
  new_free_page_node->next_free_page = kernel_memory_allocator.free_page_list_head;  // point new node to current head
  kernel_memory_allocator.free_page_list_head = new_free_page_node;        // make new node the new head  
  release(&kernel_memory_allocator.physical_memory_lock);
}
 
// allocate one 4096-byte page of physical memory from the free pool
// returns a kernel-usable pointer to the allocated page
// returns null pointer (0) if no memory is available (out of memory condition)
void * kalloc(void)
{
  struct free_page_list_node *allocated_page_node;

  // critical section: atomically remove page from free list
  // prevents race conditions when multiple cpus allocate simultaneously
  acquire(&kernel_memory_allocator.physical_memory_lock);
  allocated_page_node = kernel_memory_allocator.free_page_list_head;  // get current head of free list
  if(allocated_page_node)
    kernel_memory_allocator.free_page_list_head = allocated_page_node->next_free_page; // advance head to next node
  release(&kernel_memory_allocator.physical_memory_lock);

  // security feature: fill allocated page with garbage to catch uninitialized read bugs
  // prevents information leakage and forces code to properly initialize memory
  // different pattern (5) than kfree (1) to help distinguish allocation vs free bugs
  if(allocated_page_node)
    memset((char*)allocated_page_node, 5, PGSIZE); 
    
  return (void*)allocated_page_node;  // return pointer to allocated page (or null if out of memory)
}
