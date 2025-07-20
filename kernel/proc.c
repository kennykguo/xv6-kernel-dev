// process management implementation
//
// fundamental concepts:
// - the kernel maintains arrays of struct proc and struct cpu
// - processes are allocated from the proc[] array as needed
// - each cpu core tracks which process it's currently running
// - context switching allows rapid switching between processes
// - process IDs (PIDs) are unique identifiers assigned sequentially

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// global arrays for all cpus and processes in the system
struct cpu cpus[NCPU];    // one entry per cpu core
struct proc proc[NPROC];  // process table - all processes in system

struct proc *initproc;    // pointer to the init process (pid 1)

// pid allocation - ensures each process gets a unique identifier
int nextpid = 1;
struct spinlock pid_lock;  // protects nextpid from race conditions

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wake_ups of wait()ing parents are not lost
// helps obey the memory model when using p->parent
// must be acquired before any p->lock
struct spinlock wait_lock;

// allocate a page for each process's kernel stack
// map it high in memory, followed by an invalid guard page
// called during kernel initialization to set up kernel stacks
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  // iterate through all possible processes
  for(p = proc; p < &proc[NPROC]; p++) {
    // allocate one physical page for this process's kernel stack
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
      
    // calculate virtual address for this process's kernel stack
    // KSTACK() places stacks high in virtual memory with guard pages between them
    uint64 va = KSTACK((int) (p - proc));
    
    // map the physical page into kernel page table at calculated virtual address
    // PTE_R | PTE_W makes the stack readable and writable
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the process table
// called once during kernel startup
void procinit(void)
{
  struct proc *p;
  
  // initialize locks for process management
  init_lock(&pid_lock, "nextpid");    // protects pid allocation
  init_lock(&wait_lock, "wait_lock"); // coordinates parent/child relationships
  
  // initialize each process slot in the process table
  for(p = proc; p < &proc[NPROC]; p++) {
      init_lock(&p->lock, "proc");           // protects individual process fields
      p->state = UNUSED;                    // mark slot as available
      p->kstack = KSTACK((int) (p - proc)); // remember kernel stack virtual address
  }
}

// get the current cpu's ID (hardware thread ID)
// must be called with interrupts disabled to prevent migration to different cpu
int cpuid() {
  // read thread pointer register set during boot
  int id = r_tp();
  return id;
}

// return this cpu's cpu struct
// interrupts must be disabled to ensure we don't migrate to different cpu
struct cpu* mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];  // index into global cpu array
  return c;
}

// return the current process, or zero if none
// used throughout kernel to identify which process is running
struct proc* myproc(void)
{
  push_off();                // disable interrupts
  struct cpu *c = mycpu();   // get current cpu
  struct proc *p = c->proc;  // get process running on this cpu
  pop_off();                 // re-enable interrupts
  return p;
}

// allocate a unique process ID
// uses simple counter with spinlock for thread safety
int allocpid()
{
  int pid;
  
  acquire(&pid_lock);  // ensure atomic update of nextpid
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// look in the process table for an UNUSED proc
// if found, initialize state required to run in the kernel,
// and return with p->lock held
// if there are no free procs, or a memory allocation fails, return 0
static struct proc* allocproc(void)
{
  struct proc *p;

  // search through process table for an unused slot
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;  // found an available process slot
    } else {
      release(&p->lock);  // this slot is in use, keep searching
    }
  }
  return 0;  // no free process slots available

found:
  // initialize the new process
  p->pid = allocpid();  // assign unique process ID
  p->state = USED;      // mark as allocated but not yet runnable

  // allocate a trapframe page - holds saved user registers
  // this page will be mapped in user virtual address space
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // create an empty user page table for this process
  // each process gets its own virtual address space
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // set up new context to start executing at forkret,
  // which will return to user space
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;    // return address = forkret function
  p->context.sp = p->kstack + PGSIZE; // stack pointer = top of kernel stack

  return p;  // return new process with lock held
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wake_up(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wake_up(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// per-cpu core process scheduler - the heart of multitasking
// each cpu core calls scheduler() after initialization and never returns
// implements time-sharing by continuously switching between processes:
//  1. scan process table for runnable processes (round-robin selection)
//  2. context switch to chosen process and let it run
//  3. when process yields/blocks, switch back to scheduler and repeat
//
// this creates the illusion that multiple programs run simultaneously
// by rapidly switching between them faster than human perception
void scheduler(void)
{
  struct proc *candidate_process;
  struct cpu *current_cpu_core = mycpu();

  current_cpu_core->proc = 0;  // initialize: no process currently running on this cpu
  
  // infinite scheduling loop - scheduler runs forever
  for(;;){
    // enable interrupts to prevent deadlock scenarios
    // if all processes are sleeping/waiting, we need interrupts to wake them
    // the most recent process may have disabled interrupts during critical sections
    intr_on();

    int found_runnable_process = 0;  // flag to track if we found any work to do
    
    // scan entire process table searching for runnable processes
    // simple round-robin algorithm: fair but not optimal for performance
    // more sophisticated schedulers consider priorities, deadlines, cpu affinity
    for(candidate_process = proc; candidate_process < &proc[NPROC]; candidate_process++) {
      acquire(&candidate_process->lock);  // protect process state from concurrent cpu access
      
      if(candidate_process->state == RUNNABLE) {
        // found a process ready to execute!
        
        // transition process from ready queue to running state
        // the process is responsible for releasing its lock during execution
        // and reacquiring it when yielding back to scheduler  
        candidate_process->state = RUNNING;  // mark process as actively executing
        current_cpu_core->proc = candidate_process;         // record which process this cpu is running
        
        // perform the critical context switch operation!
        // 1. save scheduler's cpu registers (stack pointer, etc.) in current_cpu_core->context
        // 2. load process's saved registers from candidate_process->context
        // 3. execution jumps to wherever the process last yielded/was preempted
        swtch(&current_cpu_core->context, &candidate_process->context);

        // execution returns here when process yields back to scheduler
        // the process has either:
        // - voluntarily yielded (sleep, wait, exit)
        // - been preempted by timer interrupt 
        // - completed its time slice
        current_cpu_core->proc = 0;  // clear cpu's current process pointer
        found_runnable_process = 1;    // record that we successfully ran a process
      }
      release(&candidate_process->lock);
    }
    
    // handle case where no processes are currently runnable
    // all processes may be sleeping/waiting for i/o, user input, or other events
    // use wait-for-interrupt to save power until something becomes ready
    if(found_runnable_process == 0) {
      intr_on();          // ensure interrupts are enabled to wake sleeping processes
      asm volatile("wfi"); // wait for interrupt (low power mode)
    }
  }
}

// switch to scheduler - must hold only p->lock and have changed proc->state
// saves and restores interrupt enable state because intena is a property of this
// kernel thread, not this CPU. it should be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but there's no process
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  // sanity checks to catch programming errors
  if(!holding(&p->lock))
    panic("sched p->lock");       // must hold process lock
  if(mycpu()->noff != 1)
    panic("sched locks");         // must hold exactly one lock
  if(p->state == RUNNING)
    panic("sched running");       // process state must be changed before calling sched
  if(intr_get())
    panic("sched interruptible"); // interrupts must be disabled

  // save interrupt enable state and perform context switch
  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);  // switch from process to scheduler
  mycpu()->intena = intena;  // restore interrupt enable state
}

// give up the CPU for one scheduling round
// called when process wants to voluntarily yield (be nice to other processes)
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);     // protect process state
  p->state = RUNNABLE;   // mark as ready to run (not currently running)
  sched();               // switch to scheduler
  release(&p->lock);     // release lock when we resume
}

// a fork child's very first scheduling by scheduler() will swtch to forkret
// this function completes the setup of a new process
void forkret(void)
{
  static int first = 1;  // track first process creation

  // still holding p->lock from scheduler
  release(&myproc()->lock);

  if (first) {
    // file system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main()
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0
    __sync_synchronize();
  }

  // jump to user space for the first time
  usertrapret();
}

// atomically release lock and sleep on chan
// reacquires lock when awakened
// this is how processes wait for events (i/o completion, etc.)
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // must acquire p->lock in order to change p->state and then call sched
  // once we hold p->lock, we can be guaranteed that we won't miss any wake_up
  // (wake_up locks p->lock), so it's okay to release lk

  acquire(&p->lock);  // protect process state changes
  release(lk);        // release the lock we were holding

  // go to sleep
  p->chan = chan;        // remember what we're waiting for
  p->state = SLEEPING;   // mark as sleeping (not runnable)

  sched();  // switch to scheduler (process won't run until woken up)

  // when we get here, someone called wake_up(chan) and scheduler picked us to run
  // tidy up
  p->chan = 0;  // no longer waiting for anything

  // reacquire original lock before returning
  release(&p->lock);
  acquire(lk);
}

// wake up all processes sleeping on the specified channel
// scans entire process table to find sleeping processes waiting on this event
// must be called without holding any process lock to avoid deadlock
// chan - the wait channel (memory address used as event identifier)
void wake_up(void *wait_channel)
{
  struct proc *process_to_check;
  
  // scan all processes in the system process table
  for(process_to_check = proc; process_to_check < &proc[NPROC]; process_to_check++) {
    // skip the current running process (cannot wake up itself)
    if(process_to_check != myproc()){
      // acquire process lock for atomic state examination and modification
      acquire(&process_to_check->lock);
      
      // check if this process is sleeping on the specified channel
      if(process_to_check->state == SLEEPING && process_to_check->chan == wait_channel) {
        // wake up the process by making it schedulable again
        process_to_check->state = RUNNABLE;
      }
      
      release(&process_to_check->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// safely copy data to either user space or kernel space destination
// handles address space translation and memory protection automatically
// user_dst - 1 if destination is user space, 0 if kernel space
// dst - destination virtual address
// src - source kernel address (always kernel space)
// len - number of bytes to copy
// returns - 0 on success, -1 on error (invalid user address or page fault)
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *current_process = myproc();
  
  if(user_dst){
    // destination is user space - use page table translation and protection
    // copyout() checks if user address is valid and accessible
    return copyout(current_process->pagetable, dst, src, len);
  } else {
    // destination is kernel space - direct memory copy (no translation needed)
    memmove((char *)dst, src, len);
    return 0;  // kernel space access always succeeds
  }
}

// safely copy data from either user space or kernel space source
// handles address space translation and memory protection automatically  
// dst - destination kernel address (always kernel space)
// user_src - 1 if source is user space, 0 if kernel space
// src - source virtual address
// len - number of bytes to copy
// returns - 0 on success, -1 on error (invalid user address or page fault)
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *current_process = myproc();
  
  if(user_src){
    // source is user space - use page table translation and protection
    // copyin() checks if user address is valid and accessible
    return copyin(current_process->pagetable, dst, src, len);
  } else {
    // source is kernel space - direct memory copy (no translation needed)
    memmove(dst, (char*)src, len);
    return 0;  // kernel space access always succeeds
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
