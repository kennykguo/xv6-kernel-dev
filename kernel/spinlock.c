// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

// mutual exclusion spinlock
// x
void create_lock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.wq
void acquire(struct spinlock *lk)
{
  // Don't want contexst switches to take place while we want an atomic operation 
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  // This is a GCC primitive
  // Built-in atomic operations
  // If we didn't get the lock, spin forever
  // If we swapped it, then we can move beyond the lock
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;
  
  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction. Reordering up to fence, reordering to fence
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  // Are we holding the lock?
  if(!holding(lk))
    panic("release");

  // Release the lock manually
  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  // Restoring state of interrupts if necessary
  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int holding(struct spinlock *lk)
{
  int r;
  // Check if we have the lock and did the CPU actually lock it?
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void push_off(void)
{
  // Get the state of the interrupt
  int old = intr_get();

  // Actually turn off interrupts
  intr_off();

  // Keeps track of how many spinlocks are on the core
  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  // Increment the # of spinlocks on the core
  mycpu()->noff += 1;
}

void pop_off(void)
{
  // Get the current CPU
  struct cpu *c = mycpu();
  // Are interrupts enabled?
  if(intr_get())
    // Should not be here! Print error and stop running
    panic("pop_off - interruptible");
  
  if(c->noff < 1)
    panic("pop_off");
  
  // Decrement the # of spinlocks
  c->noff -= 1;
  // If spinlocks # is zero, and interrupts previously enabled
  if(c->noff == 0 && c->intena)
    intr_on();
}
