// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.


// Minimal linked list, that doesn't even contain data
struct run {
  struct run *next;
};

// Keeps track of spinlock and running list
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void kinit()
{
  initlock(&kmem.lock, "kmem");
  // 
  freerange(end, (void*)PHYSTOP);
}

// physical address start, physical address end
// Data itself is not important -> void pointer
// end is passed in as start, and PHYSTOP is passed as end -> memlayout.h
// Try to free from start to end

void freerange(void *pa_start, void *pa_end)
{
  char *p; // Pointing somewhere -> free variable mainly
  // Start at the next address that is a mult of 4096
  p = (char*)PGROUNDUP((uint64)pa_start);
  // Loop through, inc by 4K and call kfree
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// Use kalloc to get memory
void kfree(void *pa)
{
  struct run *r;

  // Invariant condition
  // Is the physical addr not a multiple of addr, or trying to free part of the kernel or its bigger than our total memory
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  // Set everything to junk
  // Userspace programs may have pointers
  memset(pa, 1, PGSIZE);

  // Cast to a pointer to run structure (physical address)
  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist; // Write the first 8 bytes to where our currently freelist is pointing to
  kmem.freelist = r; // New freelist is the current run structure
  release(&kmem.lock);
}
 
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void * kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
