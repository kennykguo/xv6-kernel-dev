// physical memory allocator, for user processes,
// kernel stacks, page-table pages, and pipe buffers. 
// allocates whole 4096-byte pages.
//
// memory management fundamental concepts:
// - physical memory is divided into 4096-byte pages
// - kernel maintains a free list of available pages  
// - kalloc() removes a page from free list and returns it
// - kernel_free_page() adds a page back to the free list
// - this is a simple but effective memory allocation strategy

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void free_memory_range(void *memory_start, void *memory_end);

// 'kernel_binary_end' symbol is defined by kernel.ld linker script
// it marks the end of the kernel binary in memory (~0x80200000 typically)
// all memory after 'kernel_binary_end' can be used for dynamic allocation
// memory layout: 0x80000000 (kernel start) -> 'kernel_binary_end' -> 0x88000000 (phystop)
extern char kernel_binary_end[]; 

// free page list node structure - space-saving technique
// instead of separate metadata, we store the free list directly
// in the free pages themselves. since free pages aren't being used,
// we can store a pointer in the first 8 bytes of each free page.
// this eliminates the need for a separate data structure to track free pages
// x
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
// sets up spinlock protection and populates initial free page pool
// x
void kernel_init_memory_allocator() {
  // initialize spinlock to protect free page list from race conditions
  // multiple cpu cores may call kalloc/kernel_free_page simultaneously during operation
  create_lock(&kernel_memory_allocator.physical_memory_lock, "physical_memory_allocator");
  
  // populate free list with all available physical memory pages
  // memory from kernel_binary_end to PHYSTOP (physical memory limit) is available
  // this creates the initial pool of allocatable pages
  free_memory_range(kernel_binary_end, (void*)PHYSTOP);
}

// add all pages in range [memory_start, memory_end) to free list
// called during initialization to populate the initial free page pool
// takes a contiguous block of physical memory and breaks it into 4kb pages
// x
void free_memory_range(void *memory_start, void *memory_end)
{
  char *current_page_address;
  
  // round up start address to next page boundary
  // ensures we only free complete 4096-byte aligned pages
  current_page_address = (char*)PGROUNDUP((uint64)memory_start);
  
  // iterate through each 4kb page in the specified range
  for(; current_page_address + PGSIZE <= (char*)memory_end; current_page_address += PGSIZE)
    kernel_free_page(current_page_address); // add this page to the free list
}

// return a physical memory page to the free pool
// accepts a page address that was previously allocated by kalloc()
// (exception: during initialization, this is called on never-allocated pages)
// x
void kernel_free_page(void *page_address)
{
  struct free_page_list_node *new_free_page_node;

  // comprehensive sanity checks to catch programming errors:
  // 1. page_address must be page-aligned (multiple of 4096 bytes)
  // 2. page_address must not point into kernel code/data section
  // 3. page_address must be within valid physical memory range
  if(((uint64)page_address % PGSIZE) != 0 || 
     (char*)page_address < kernel_binary_end || 
     (uint64)page_address >= PHYSTOP)
    panic("kernel_free_page");

  // security feature - fill page with garbage to catch use-after-free bugs
  // if code accidentally tries to use freed memory, it gets junk instead of old data
  // this debugging technique helps catch dangling pointer vulnerabilities
  // preset the page to be filled with 1s
  memset(page_address, 1, PGSIZE);

  // prepare to add page to front of free list
  // treat the freed page itself as a free_page_list_node structure
  // free_page_list_node has only a pointer to the next page
  new_free_page_node = (struct free_page_list_node*)page_address;

  // critical section - update free list atomically to prevent race conditions
  // basically adding node to front of linked list, then updating head
  acquire(&kernel_memory_allocator.physical_memory_lock);
  new_free_page_node->next_free_page = kernel_memory_allocator.free_page_list_head; // point new node to current head
  kernel_memory_allocator.free_page_list_head = new_free_page_node; // make new node the new head  
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
  // different pattern (5) than kernel_free_page (1) to help distinguish allocation vs free bugs
  if(allocated_page_node)
    memset((char*)allocated_page_node, 5, PGSIZE); 
    
  return (void*)allocated_page_node;  // return pointer to allocated page (or null if out of memory)
}
