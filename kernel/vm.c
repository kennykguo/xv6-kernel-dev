// virtual memory management
//
// virtual memory fundamental concepts:
// - every process has its own virtual address space
// - virtual addresses are translated to physical addresses by the mmu (memory management unit)
// - translation is controlled by page tables - data structures that map virtual to physical addresses
// - page tables enable memory protection, isolation between processes, and efficient memory use
//
// risc-v sv39 virtual memory system:
// - 39-bit virtual addresses (512 GB virtual address space)  
// - 3-level page table hierarchy
// - 4KB pages (4096 bytes per page)
// - page table entries (PTEs) contain physical address + permission bits

#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

// the kernel's page table - maps kernel virtual addresses to physical addresses
// kernel runs in virtual memory just like user processes
pagetable_t kernel_pagetable;
// 512 PTEs

// linker symbols marking sections of kernel binary
extern char etext[];  // kernel.ld sets this to end of kernel code.
extern char trampoline[]; // trampoline.S - assembly code for kernel entry/exit

// create a direct-map page table for the kernel
// "direct-map" means virtual address = physical address for most mappings
// this simplifies kernel memory management
pagetable_t create_kernel_page_table(void)
{
  pagetable_t kernel_root_page_table;

  // allocate a physical page to hold the root page table
  // page table is just an array of 512 64-bit entries (4KB total)
  kernel_root_page_table = (pagetable_t) kalloc();
  memset(kernel_root_page_table, 0, PGSIZE); // clear all entries (invalid by default)


  // set up kernel mappings for memory-mapped devices
  // these mappings allow kernel to access hardware by reading/writing memory
  
  // uart registers - for console i/o
  map_kernel_virtual_to_physical(kernel_root_page_table, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface - for file system storage
  map_kernel_virtual_to_physical(kernel_root_page_table, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC (platform-level interrupt controller) - for handling device interrupts
  // large mapping (64MB) covers all PLIC registers
  map_kernel_virtual_to_physical(kernel_root_page_table, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text (code) as executable and read-only.
  // etext symbol marks end of kernel code section
  // PTE_X makes pages executable, PTE_R makes them readable
  map_kernel_virtual_to_physical(kernel_root_page_table, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and remaining physical RAM as read-write
  // everything after kernel code can be read and written but not executed
  map_kernel_virtual_to_physical(kernel_root_page_table, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline page to highest virtual address in kernel space
  // trampoline contains assembly code for switching between user/kernel mode
  // it must be mapped at same virtual address in user and kernel page tables
  map_kernel_virtual_to_physical(kernel_root_page_table, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  // each process gets its own kernel stack for use when running in kernel mode
  allocate_and_map_process_kernel_stacks(kernel_root_page_table);
  
  return kernel_root_page_table;
}



// initialize the kernel page table
// called once during kernel startup
void initialize_kernel_virtual_memory(void)
{
  kernel_pagetable = create_kernel_page_table();
}


// switch hardware page table register to the kernel's page table,
// and enable paging.
// this is where virtual memory actually gets turned on!
void kvminithart()
{
  // memory barrier - ensure all previous writes to page table are visible
  // important for correctness on out-of-order cpus
  sfence_vma();

  // load page table root address into satp (supervisor address translation and protection)
  // MAKE_SATP creates the proper satp value with mode bits
  // after this instruction, virtual memory is ENABLED
  write_supervisor_address_translation_and_protection_register(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB (translation lookaside buffer)
  // TLB caches recent virtual-to-physical address translations
  // must flush when changing page tables
  sfence_vma();
}

// return the address of the PTE in page table pagetable
// that corresponds to virtual address va. if alloc!=0,
// create any required page-table pages.
//
// the risc-v sv39 scheme has three levels of page-table pages:
// - level 2: top-level page directory (root)
// - level 1: middle-level page tables  
// - level 0: bottom-level page tables (leaf)
//
// a page-table page contains 512 64-bit PTEs.
// a 64-bit virtual address is split into five fields:
//   39..63 -- must be zero (unused)
//   30..38 -- 9 bits of level-2 index (512 entries)
//   21..29 -- 9 bits of level-1 index (512 entries)  
//   12..20 -- 9 bits of level-0 index (512 entries)
//    0..11 -- 12 bits of byte offset within the page (4096 bytes)
//
// this gives 512 * 512 * 512 * 4096 = 512 GB virtual address space
pte_t * walk_page_table(pagetable_t page_table_root, uint64 virtual_address, int should_allocate_missing_tables)
{
  // check if virtual address is within valid range
  if(virtual_address >= MAXVA)
    panic("walk_page_table not in valid range");

  // walk down the 3-level page table hierarchy
  // start at level 2 (root), walk down to level 0 (leaf)
  for(int current_page_table_level = 2; current_page_table_level > 0; current_page_table_level--) {
    // get pointer to page table entry for this level
    // PX() extracts the appropriate 9-bit index from virtual address
    pte_t* current_level_page_table_entry = &page_table_root[PX(current_page_table_level, virtual_address)];
    
    if(*current_level_page_table_entry & PTE_V) {
      // page table entry is valid - extract physical address of next level
      page_table_root = (pagetable_t)PTE2PA(*current_level_page_table_entry);
    } 
    else {
      // page table entry is invalid - need to allocate new page table
      if(!should_allocate_missing_tables || (page_table_root = (pde_t*)kalloc()) == 0){
        printf("not allocating page table bcz invalid entry");
        return 0; // allocation failed or not requested
      }
        
      // clear new page table (all entries start invalid)
      memset(page_table_root, 0, PGSIZE);

      // install physical address of new page table in current page table entry
      // set valid bit to make this entry active
      *current_level_page_table_entry = PA2PTE(page_table_root) | PTE_V;
    }
  }
  
  // return pointer to final level-0 page table entry that maps the virtual address
  return &page_table_root[PX(0, virtual_address)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk_page_table(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table
// only used when booting.
// does not flush TLB or enable paging
void map_kernel_virtual_to_physical(pagetable_t kernel_page_table, uint64 virtual_address, uint64 physical_address, uint64 mapping_size, int page_permissions)
{
  if(create_page_table_mappings(kernel_page_table, virtual_address, mapping_size, physical_address, page_permissions) != 0)
    panic("map_kernel_virtual_to_physical");
}

// create page table entries for virtual addresses starting at virtual_address_start that refer to physical addresses starting at physical_address_start
// virtual_address_start and total_mapping_size MUST be page-aligned
// returns 0 on success, -1 if walk_page_table() couldn't allocate a needed page-table page
// x
int create_page_table_mappings(pagetable_t page_table, uint64 virtual_address_start, uint64 total_mapping_size, uint64 physical_address_start, int page_permissions)
{
  uint64 current_virtual_address, last_virtual_address;
  pte_t *current_page_table_entry;

  if((virtual_address_start % PGSIZE) != 0)
    panic("create_page_table_mappings: va not aligned");

  if((total_mapping_size % PGSIZE) != 0)
    panic("create_page_table_mappings: size not aligned");

  if(total_mapping_size == 0)
    panic("create_page_table_mappings: size");
     
  current_virtual_address = virtual_address_start;
  last_virtual_address = virtual_address_start + total_mapping_size - PGSIZE; // each entry should hold a page worth, thats why we subtract pgsize
  
  // iterate through each page in the mapping range
  while(1) {
    // find the page table entry for this virtual address
    // allocate intermediate page tables if needed (parameter 1 means allocate)
    current_page_table_entry = walk_page_table(page_table, current_virtual_address, 1);
    if(current_page_table_entry == 0) {
      return -1; // if walk couldn't allocate needed page tables, return error
    }
    
    // check if this virtual address is already mapped or page table entry pointer now points to a valid page table entry
    if(*current_page_table_entry & PTE_V) {
      panic("virtual address is already mapped");
    }
    
    // create the mapping by setting the page table entry
    // PA2PTE converts physical address to page table entry format
    // combine with permissions and valid bit
    *current_page_table_entry = PA2PTE(physical_address_start) | page_permissions | PTE_V;
    
    // check if we've mapped all requested pages
    if(current_virtual_address == last_virtual_address) {
      break; // finished mapping all pages
    }
    
    // advance to next page
    current_virtual_address += PGSIZE;  // move to next 4KB virtual page
    physical_address_start += PGSIZE;   // move to next 4KB physical page
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk_page_table(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kernel_free_page((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  create_page_table_mappings(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(create_page_table_mappings(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kernel_free_page(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kernel_free_page((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk_page_table(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(create_page_table_mappings(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kernel_free_page(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk_page_table(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
    pte = walk_page_table(pagetable, va0, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
       (*pte & PTE_W) == 0)
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
