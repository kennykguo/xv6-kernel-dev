//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>
#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

// console character output - the bridge between printf and uart hardware
// this function handles special characters (like backspace) and forwards everything to uart
// called by printf() for all formatted output and by kernel for echoing input characters
// this is where printf output ultimately flows to reach the console
void consputc(int character_to_output)
{
  // handle backspace character specially for proper terminal behavior
  // backspace (0x100) needs to move cursor back, erase character, then move cursor back again
  // this creates the visual effect of deleting the previous character on the terminal
  if(character_to_output == BACKSPACE){
    // three-step backspace sequence for proper terminal control:
    // 1. '\b' moves cursor one position left
    // 2. ' ' (space) overwrites the character with blank space  
    // 3. '\b' moves cursor back to the space we just wrote
    // this effectively erases the previous character and positions cursor correctly
    uartputc_sync('\b'); 
    uartputc_sync(' '); 
    uartputc_sync('\b');
  } else {
    // for all other characters, send directly to uart hardware
    // this includes regular text, newlines, tabs, and other control characters
    // uartputc_sync handles the low-level uart communication synchronously
    uartputc_sync(character_to_output);
  }
}

struct {
  struct spinlock lock;
  // input
  #define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

// write characters from user space to console output
// copies n bytes from user buffer to console, one character at a time
// user_src - 1 if source is user space, 0 if kernel space
// src - source address in user/kernel space 
// n - number of bytes to write
// returns - number of bytes successfully written
// x
int console_write(int user_src, uint64 src, int n)
{
  int bytes_written;

  // process each byte individually to handle memory access errors gracefully
  for(bytes_written = 0; bytes_written < n; bytes_written++){
    char current_char;
    
    // safely copy one byte from user/kernel space to kernel buffer
    // either_copyin handles address space translation and protection
    int copy_result = either_copyin(&current_char, user_src, src + bytes_written, 1);
    if(copy_result == -1){
      break;  // memory access failed - stop writing and return partial count
    }
    
    // send character to uart hardware for immediate display
    // uart_put_char may block if transmit buffer is full
    uart_put_char(current_char);
  }

  return bytes_written;  // return number of bytes successfully written
}

// read characters from console input buffer to user space
// copies up to n bytes from console input buffer to user buffer
// blocks until at least one character is available or newline received
// user_dst - 1 if destination is user space, 0 if kernel space
// dst - destination address in user/kernel space
// n - maximum number of bytes to read
// returns - number of bytes successfully read, or -1 if process killed
// x
int console_read(int user_dst, uint64 dst, int n)
{
  uint bytes_requested;
  int current_char;
  char char_buffer;

  bytes_requested = n;
  acquire(&cons.lock);
  
  while(n > 0){
    // wait until interrupt handler has put some input into console buffer
    // cons.r == cons.w means buffer is empty (read index equals write index)
    while(cons.r == cons.w){
      // check if process was killed while waiting for input
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      // sleep until console_intr() wakes us up with new input
      sleep(&cons.r, &cons.lock);
    }

    // get next character from circular input buffer
    current_char = cons.buf[cons.r % INPUT_BUF_SIZE];
    cons.r++;  // advance read index to next character 

    // handle ctrl-d (end of file) - unix convention for eof
    if(current_char == C('D')){
      // if we have already read some data, save ctrl-d for next call
      // this ensures proper eof behavior - first read returns partial data,
      // second read returns 0 bytes to signal end of file
      if(n < bytes_requested){
        cons.r--;  // put ctrl-d back in buffer for next read
      }
      break;  // stop reading and return what we have so far
    }

    // copy character from kernel buffer to user space buffer
    char_buffer = current_char;
    if(either_copyout(user_dst, dst, &char_buffer, 1) == -1)
      break;  // copy failed - user memory access error

    dst++;  // advance destination pointer to next byte position
    n--;    // decrease remaining bytes to read

    // stop reading when we hit newline - this implements line-buffered input
    // user programs typically read one line at a time from console
    if(current_char == '\n'){
      break;
    }
  }
  
  release(&cons.lock);
  return bytes_requested - n;  // return number of bytes actually read
}

//
// the console input interrupt handler.
// uartintr() calls this for input character.
// do erase/kill processing, append to cons.buf,
// wake up console_read() if a whole line has arrived.
//
void console_intr(int c)
{
  // Get the lock
  // UART is memory mapped
  // Any threads that don't get the lock stay here
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by console_read().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up console_read() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wake_up(&cons.r);
      }
    }
    break;
  }
  
  // Releasing the lock
  release(&cons.lock);
}



// initialize console hardware and connect it to the file system
// x
void console_init(void)
{
  // initialize the console lock to synchronize access between cpus
  // this func creates the lock for the console lock struct
  create_lock(&cons.lock, "cons");

  // initialize the uart hardware for serial communication
  // uart (universal asynchronous receiver/transmitter) handles serial i/o
  // this sets up the 16550a uart chip emulated by qemu
  uart_init(); 

  // connect read and write system calls to console functions
  // device_drivers is the device driver table that maps device numbers to driver functions
  // this makes the console accessible as file descriptor 0, 1, 2 (stdin, stdout, stderr)
  device_drivers[CONSOLE].read = console_read;
  device_drivers[CONSOLE].write = console_write;
}
