// system parameters - fundamental limits and configuration values
//
// these constants define the basic limits of the xv6 system
// they determine how many resources can be used simultaneously
// increasing these values allows more processes/files but uses more memory

// process and cpu limits
#define NPROC        64  // maximum number of processes in the system
                         // each process uses ~4KB for struct proc + other memory
#define NCPU          8  // maximum number of CPU cores supported  
                         // xv6 can run on multiprocessor systems up to this limit

// file system limits
#define NOFILE       16  // open files per process (file descriptors 0-15)
                         // limits how many files one process can have open
#define NFILE       100  // total open files across all processes
                         // kernel maintains a global file table with this many entries
#define NINODE       50  // maximum number of active i-nodes in memory
                         // i-nodes contain file/directory metadata

// device and storage limits  
#define NDEV         10  // maximum major device number
                         // devices are identified by major/minor number pairs
#define ROOTDEV       1  // device number of file system root disk
                         // tells kernel which device contains the root file system

// program execution limits
#define MAXARG       32  // max arguments to exec() system call
                         // limits command line arguments when starting programs
#define MAXPATH      128 // maximum file path name length
                         // longest allowed file/directory path

// file system implementation limits
#define MAXOPBLOCKS  10  // max blocks any single file system operation writes
                         // ensures atomic operations don't consume too much log space
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
                                      // crash recovery log for atomic file operations  
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
                                      // how many disk blocks to cache in memory
#define FSSIZE       2000  // size of file system in blocks (each block = 1KB)
                          // total storage capacity of the file system

// user program limits
#define USERSTACK    1     // user stack pages (4KB per page)
                          // how much stack space user programs get

