// on-disk file system format
// both the kernel and user programs use this header file
//
// file system fundamental concepts:
// - persistent storage is organized into fixed-size blocks (1024 bytes each)
// - files are collections of blocks containing data
// - directories are special files containing lists of other files
// - inodes (index nodes) store metadata about files (size, location, permissions)
// - the file system provides a hierarchical namespace (/usr/bin/ls)
//
// xv6 uses a simple unix-like file system with these components:
// - superblock: describes the overall file system layout
// - inodes: metadata for each file/directory  
// - data blocks: actual file contents
// - free bitmap: tracks which blocks are allocated
// - log: for crash recovery and atomic operations

#define ROOTINO  1   // root directory inode number (/ is always inode 1)
#define BSIZE 1024   // block size in bytes (each disk block is 1KB)

// disk layout - how blocks are organized on the storage device:
// [ boot block | super block | log | inode blocks | free bit map | data blocks]
//
// boot block (block 0): contains boot loader code (not used by file system)
// super block (block 1): describes file system layout and parameters
// log blocks: for crash recovery - staged writes for atomic operations  
// inode blocks: contain on-disk inode structures (file metadata)
// free bit map: bitmap tracking which data blocks are free/allocated
// data blocks: actual file contents and directory data

// mkfs computes the super block and builds an initial file system
// the super block describes the disk layout and contains key parameters
struct superblock {
  uint magic;        // magic number (FSMAGIC) to identify valid file system
  uint size;         // total size of file system image (blocks)
  uint nblocks;      // number of data blocks available for files
  uint ninodes;      // number of inodes (max number of files/directories)
  uint nlog;         // number of log blocks for crash recovery
  uint logstart;     // block number where log begins
  uint inodestart;   // block number where inode blocks begin
  uint bmapstart;    // block number where free bitmap begins
};

#define FSMAGIC 0x10203040  // magic number to identify xv6 file systems

// file size limits based on inode structure
#define NDIRECT 12                              // number of direct block addresses in inode
#define NINDIRECT (BSIZE / sizeof(uint))        // number of indirect block addresses (256)
#define MAXFILE (NDIRECT + NINDIRECT)           // maximum file size in blocks (268 blocks = 268KB)

// on-disk inode structure - stored permanently on disk
// each file/directory has exactly one inode containing its metadata
struct dinode {
  short type;           // file type: T_FILE, T_DIR, T_DEVICE, or 0 (free)
  short major;          // major device number (for device files only)  
  short minor;          // minor device number (for device files only)
  short nlink;          // number of directory entries pointing to this inode
  uint size;            // size of file content in bytes
  uint addrs[NDIRECT+1]; // block addresses: first 12 are direct, last is indirect
                        // direct addresses point to data blocks
                        // indirect address points to block containing more addresses
};

// utility macros for inode and block calculations

// inodes per block - how many inodes fit in one disk block
#define IPB           (BSIZE / sizeof(struct dinode))

// block containing inode i - which block on disk contains this inode
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// bitmap bits per block - how many free/allocated bits fit in one bitmap block  
#define BPB           (BSIZE*8)

// block of free map containing bit for block b - which bitmap block tracks this data block
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// directory structure - directories are files with special format
// a directory contains a sequence of directory entries (dirents)
// each entry maps a filename to an inode number
#define DIRSIZ 14  // maximum filename length (including null terminator)

struct dirent {
  ushort inum;        // inode number (0 means this entry is free)
  char name[DIRSIZ];  // filename (null-terminated string)
};

