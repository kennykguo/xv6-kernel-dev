// in-memory file system structures and device interface
//
// the kernel maintains in-memory structures that mirror and cache on-disk data:
// - struct file: represents an open file (file descriptor)
// - struct inode: in-memory copy of disk inode with additional state
// - struct device_driver: device driver interface for special files

// open file structure - represents one file descriptor
// when a process opens a file, kernel creates one of these structures
// multiple file descriptors can point to the same file
struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;  // what kind of file descriptor
  int ref;        // reference count - how many file descriptors point to this file
  char readable;  // is this file open for reading?
  char writable;  // is this file open for writing?
  struct pipe *pipe;   // pipe structure (for FD_PIPE type)
  struct inode *ip;    // inode pointer (for FD_INODE and FD_DEVICE types)  
  uint off;           // current file offset/position (for FD_INODE type)
  short major;        // major device number (for FD_DEVICE type)
};

// device number manipulation macros
// device numbers identify hardware devices (disk, console, etc.)
// major number identifies device type, minor number identifies specific device
#define major(dev)  ((dev) >> 16 & 0xFFFF)  // extract major device number
#define minor(dev)  ((dev) & 0xFFFF)        // extract minor device number  
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))   // combine major/minor into device number

// in-memory copy of an inode
// the kernel caches frequently used inodes in memory for performance
// in-memory inodes contain additional state not stored on disk
struct inode {
  uint dev;           // device number containing this inode
  uint inum;          // inode number on device
  int ref;            // reference count - how many pointers reference this inode
  struct sleeplock lock; // protects all fields below (allows blocking while holding lock)
  int valid;          // has inode been read from disk? (0=no, 1=yes)

  // cached copy of disk inode - these fields mirror struct dinode
  short type;         // file type: T_FILE, T_DIR, T_DEVICE, or 0 (free)
  short major;        // major device number (for device files)
  short minor;        // minor device number (for device files)
  short nlink;        // number of directory entries pointing to this inode
  uint size;          // size of file content in bytes
  uint addrs[NDIRECT+1]; // block addresses (see fs.h for explanation)
};

// device driver interface - maps major device numbers to driver functions
// allows kernel to support different types of devices (console, disk, etc.)
// each device type provides read and write functions
struct device_driver {
  int (*read)(int, uint64, int);   // device read function pointer
  int (*write)(int, uint64, int);  // device write function pointer
};

extern struct device_driver device_drivers[];  // global device driver table

// well-known device numbers
#define CONSOLE 1  // major device number for console (terminal)
