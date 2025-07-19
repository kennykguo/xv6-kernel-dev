// system call implementation and dispatch
//
// system calls provide the interface between user programs and kernel
// when user code executes 'ecall' instruction:
// 1. hardware switches to supervisor mode
// 2. hardware jumps to trap handler (usertrap)  
// 3. usertrap calls syscall() to dispatch based on system call number
// 4. syscall() looks up and calls the appropriate kernel function
// 5. return value is placed in a0 register for user program
//
// key concepts:
// - user and kernel run in different address spaces (different page tables)
// - kernel must carefully copy data to/from user space (copyin/copyout)
// - arguments are passed in risc-v argument registers (a0-a5)
// - system call number is passed in a7 register

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// fetch a 64-bit value from user virtual address
// safely copies data from user space to kernel space
int fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  // check if address is within process memory bounds
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    return -1;
  // use copyin to safely copy from user page table to kernel
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// fetch a null-terminated string from user virtual address
// returns length of string, not including null terminator, or -1 for error
int fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  // copyinstr safely copies string from user space, stopping at null terminator
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

// get the nth system call argument from saved registers
// arguments are passed in risc-v argument registers a0-a5
static uint64 argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;  // first argument in a0 register
  case 1:
    return p->trapframe->a1;  // second argument in a1 register
  case 2:
    return p->trapframe->a2;  // third argument in a2 register
  case 3:
    return p->trapframe->a3;  // fourth argument in a3 register
  case 4:
    return p->trapframe->a4;  // fifth argument in a4 register
  case 5:
    return p->trapframe->a5;  // sixth argument in a5 register
  }
  panic("argraw");
  return -1;
}

// fetch the nth 32-bit system call argument
void argint(int n, int *ip)
{
  *ip = argraw(n);
}

// retrieve an argument as a pointer (virtual address)
// doesn't check for legality, since copyin/copyout will do that
void argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// fetch the nth word-sized system call argument as a null-terminated string
// copies into buf, at most max characters
// returns string length if OK (including null), -1 if error
int argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);  // get string address from argument
  return fetchstr(addr, buf, max);  // safely copy string from user space
}

// function prototypes for system call implementations
// these are the actual kernel functions that implement each system call
extern uint64 sys_fork(void);    // process creation
extern uint64 sys_exit(void);    // process termination
extern uint64 sys_wait(void);    // wait for child
extern uint64 sys_pipe(void);    // create pipe
extern uint64 sys_read(void);    // read from file descriptor
extern uint64 sys_kill(void);    // kill process
extern uint64 sys_exec(void);    // execute program
extern uint64 sys_fstat(void);   // get file status
extern uint64 sys_chdir(void);   // change directory
extern uint64 sys_dup(void);     // duplicate file descriptor
extern uint64 sys_getpid(void);  // get process ID
extern uint64 sys_sbrk(void);    // grow/shrink memory
extern uint64 sys_sleep(void);   // sleep for time
extern uint64 sys_uptime(void);  // get uptime
extern uint64 sys_open(void);    // open file
extern uint64 sys_write(void);   // write to file descriptor
extern uint64 sys_mknod(void);   // make device node
extern uint64 sys_unlink(void);  // unlink file
extern uint64 sys_link(void);    // link file
extern uint64 sys_mkdir(void);   // make directory
extern uint64 sys_close(void);   // close file descriptor

// system call dispatch table - maps system call numbers to handler functions
// this array is indexed by system call number to find the correct function
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
};

// main system call dispatcher
// called from usertrap() when a system call trap occurs  
void syscall(void)
{
  int num;
  struct proc *p = myproc();

  // system call number is passed in a7 register
  num = p->trapframe->a7;
  
  // validate system call number and dispatch to handler
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // call the system call handler function and store return value in a0
    // user program will see this return value when system call completes
    p->trapframe->a0 = syscalls[num]();
  } else {
    // invalid system call number
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;  // return error
  }
}
