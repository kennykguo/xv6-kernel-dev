// process management data structures
//
// fundamental concepts:
// - a process is a running program with its own memory space and execution state
// - the kernel manages multiple processes through time-sharing (scheduling)  
// - each process has saved registers, memory, and other state information
// - context switching allows the cpu to switch between processes rapidly
//
// key structures:
// - struct proc: represents one process
// - struct cpu: represents one cpu core's state
// - struct context: saved registers for kernel context switches
// - struct trapframe: saved registers when entering kernel from user space

// saved registers for kernel context switches
// when the scheduler switches between processes, it saves/restores these registers
// this is the minimal set needed to resume execution exactly where it left off
struct context {
  uint64 ra;        // return address - where to continue executing
  uint64 sp;        // stack pointer - current stack location

  // callee-saved registers (s0-s11)
  // risc-v calling convention says these must be preserved across function calls
  // so the context switch code must save/restore them
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// per-CPU state - each cpu core has one of these structures
// in a multiprocessor system, multiple cpus can run different processes simultaneously
struct cpu {
  struct proc *proc;          // the process currently running on this cpu (or null)
  struct context context;     // saved registers for scheduler context switches
  int noff;                   // depth of push_off() nesting for interrupt control
  int intena;                 // were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S
// when a user process enters the kernel (system call, interrupt, exception),
// all user registers must be saved so they can be restored later
// the trapframe sits in a dedicated page in the user's virtual address space
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table - loaded when entering kernel
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // address of kernel trap handler (usertrap)
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel thread pointer (cpu id)
  
  // all risc-v general-purpose registers (32 total)
  // these hold the user program's state when it was interrupted
  /*  40 */ uint64 ra;    // return address
  /*  48 */ uint64 sp;    // stack pointer  
  /*  56 */ uint64 gp;    // global pointer
  /*  64 */ uint64 tp;    // thread pointer
  /*  72 */ uint64 t0;    // temporary registers
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;    // saved registers
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;    // argument/return registers
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;    // more saved registers
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;    // more temporary registers
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

// process states - a process transitions through these states during its lifetime
enum procstate { 
  UNUSED,     // process slot is available
  USED,       // process is being set up (transitional state)  
  SLEEPING,   // process is blocked waiting for some event
  RUNNABLE,   // process is ready to run but not currently running
  RUNNING,    // process is currently executing on a cpu
  ZOMBIE      // process has exited but parent hasn't collected exit status
};

// per-process state - the complete information about one process
// this structure contains everything the kernel needs to manage a process
struct proc {
  struct spinlock lock;        // protects process fields from concurrent access

  // these fields must be protected by p->lock:
  enum procstate state;        // current process state (see enum above)
  void *chan;                  // if sleeping, what event are we waiting for?
  int killed;                  // if non-zero, process should exit
  int xstate;                  // exit status to be returned to parent's wait()
  int pid;                     // process ID (unique identifier)

  // wait_lock must be held when using this:
  struct proc *parent;         // parent process (for wait/exit communication)

  // these are private to the process, so p->lock need not be held:
  uint64 kstack;               // virtual address of kernel stack
  uint64 sz;                   // size of process memory (bytes)
  pagetable_t pagetable;       // user page table (virtual memory mappings)
  struct trapframe *trapframe; // saved user registers (page for trampoline.S)
  struct context context;      // saved kernel registers (for scheduler switches)
  struct file *ofile[NOFILE];  // open files (file descriptors)
  struct inode *cwd;           // current working directory
  char name[16];               // process name (for debugging)
};
