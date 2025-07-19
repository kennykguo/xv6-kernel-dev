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

// helps ensure that wakeups of wait()ing parents are not lost
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
  initlock(&pid_lock, "nextpid");    // protects pid allocation
  initlock(&wait_lock, "wait_lock"); // coordinates parent/child relationships
  
  // initialize each process slot in the process table
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");           // protects individual process fields
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
      wakeup(initproc);
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
  wakeup(p->parent);
  
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

// per-CPU process scheduler
// each CPU calls scheduler() after setting itself up
// scheduler never returns - it loops forever doing:
//  - choose a process to run (round-robin selection)
//  - swtch to start running that process  
//  - eventually that process transfers control back via swtch to the scheduler
//
// this is the heart of the operating system - it implements multitasking
// by rapidly switching between processes, giving each one a time slice
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;  // no process currently running on this cpu
  
  // infinite loop - scheduler runs forever
  for(;;){
    // enable interrupts to avoid deadlock if all processes are waiting
    // the most recent process to run may have had interrupts turned off
    intr_on();

    int found = 0;  // track whether we found a runnable process
    
    // scan through entire process table looking for runnable processes
    // this is simple round-robin scheduling - not sophisticated but fair
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);  // protect process state from concurrent modification
      
      if(p->state == RUNNABLE) {
        // found a process ready to run!
        
        // switch to chosen process - it is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us
        p->state = RUNNING;  // mark process as currently running
        c->proc = p;         // tell this cpu which process it's running
        
        // perform the context switch!
        // save scheduler's registers in c->context
        // load process's registers from p->context
        // execution continues wherever the process last yielded
        swtch(&c->context, &p->context);

        // when we get here, the process has yielded back to scheduler
        // process is done running for now
        // it should have changed its p->state before coming back
        c->proc = 0;  // no longer running any process
        found = 1;    // we found and ran a process
      }
      release(&p->lock);
    }
    
    // if no runnable processes found, wait for interrupt
    // wfi (wait for interrupt) puts cpu in low-power state until interrupt arrives
    // interrupts might wake up sleeping processes or signal completion of i/o
    if(found == 0) {
      intr_on();          // ensure interrupts are enabled
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
  // once we hold p->lock, we can be guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock), so it's okay to release lk

  acquire(&p->lock);  // protect process state changes
  release(lk);        // release the lock we were holding

  // go to sleep
  p->chan = chan;        // remember what we're waiting for
  p->state = SLEEPING;   // mark as sleeping (not runnable)

  sched();  // switch to scheduler (process won't run until woken up)

  // when we get here, someone called wakeup(chan) and scheduler picked us to run
  // tidy up
  p->chan = 0;  // no longer waiting for anything

  // reacquire original lock before returning
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
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

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
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
