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

// free list node structure - elegant trick!
// instead of separate metadata, we store the free list directly
// in the free pages themselves. since free pages aren't being used,
// we can store a pointer in the first 8 bytes of each free page.
struct run {
  struct run *next; // pointer to next free page
};

// global allocator state
struct {
  struct spinlock lock;  // protects freelist from concurrent access
  struct run *freelist;  // head of free page list
} kmem;

// initialize the memory allocator
// called once during kernel startup
void kinit()
{
  // initialize spinlock to protect freelist from race conditions
  // multiple cpus may call kalloc/kfree simultaneously  
  initlock(&kmem.lock, "kmem");
  
  // add all free memory pages to the free list
  // memory from end of kernel to PHYSTOP is available for allocation
  freerange(end, (void*)PHYSTOP);
}

// add all pages in range [pa_start, pa_end) to free list
// called during initialization to populate the free list
void freerange(void *pa_start, void *pa_end)
{
  char *p;
  
  // round up start address to next page boundary
  // ensures we only free complete 4096-byte pages
  p = (char*)PGROUNDUP((uint64)pa_start);
  
  // iterate through each page in the range
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p); // add this page to free list
}

// free the page of physical memory pointed at by pa,
// which normally should have been returned by a call to kalloc().
// (the exception is when initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  // sanity checks to catch programming errors:
  // - pa must be page-aligned (multiple of 4096)  
  // - pa must not point into kernel code/data
  // - pa must be within our memory range
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // fill page with junk to catch use-after-free bugs
  // if code tries to use freed memory, it gets garbage instead of old data
  // this helps catch dangling pointer bugs during development
  memset(pa, 1, PGSIZE);

  // add page to front of free list
  // treat the page itself as a struct run
  r = (struct run*)pa;

  // critical section: update free list atomically
  acquire(&kmem.lock);
  r->next = kmem.freelist;  // point new node to current head
  kmem.freelist = r;        // make new node the new head  
  release(&kmem.lock);
}
 
// allocate one 4096-byte page of physical memory.
// returns a pointer that the kernel can use.
// returns 0 if the memory cannot be allocated.
void * kalloc(void)
{
  struct run *r;

  // critical section: remove page from free list atomically
  acquire(&kmem.lock);
  r = kmem.freelist;        // get current head of free list
  if(r)
    kmem.freelist = r->next; // advance head to next node
  release(&kmem.lock);

  // fill allocated page with junk to catch uninitialized read bugs
  // forces code to properly initialize memory before using it
  if(r)
    memset((char*)r, 5, PGSIZE); 
    
  return (void*)r;  // return pointer to allocated page (or 0 if no memory)
}
