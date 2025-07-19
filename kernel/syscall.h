// system call numbers - unique identifiers for each system call
//
// system calls are the interface between user programs and the kernel
// when a user program needs kernel services, it uses the ecall instruction
// with a system call number in register a7 to identify which service it wants
//
// each system call has a unique number that maps to a specific kernel function
// this provides a stable interface that user programs can rely on

// process management system calls
#define SYS_fork    1   // create a new process (copy of current process)
#define SYS_exit    2   // terminate current process  
#define SYS_wait    3   // wait for child process to exit
#define SYS_getpid 11   // get current process ID
#define SYS_kill    6   // send signal to process
#define SYS_exec    7   // replace current process with new program
#define SYS_sleep  13   // sleep for specified number of timer ticks

// file system calls
#define SYS_open   15   // open file and return file descriptor
#define SYS_read    5   // read data from file descriptor
#define SYS_write  16   // write data to file descriptor  
#define SYS_close  21   // close file descriptor
#define SYS_dup    10   // duplicate file descriptor
#define SYS_pipe    4   // create pipe for inter-process communication

// file/directory manipulation
#define SYS_chdir   9   // change current working directory
#define SYS_mkdir  20   // create directory
#define SYS_mknod  17   // create device file
#define SYS_unlink 18   // remove file/directory
#define SYS_link   19   // create hard link to file

// information and status calls  
#define SYS_fstat   8   // get file information
#define SYS_uptime 14   // get system uptime in ticks

// memory management
#define SYS_sbrk   12   // grow/shrink process memory
