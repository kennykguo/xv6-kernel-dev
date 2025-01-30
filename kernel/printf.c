//
// formatted console output -- printf, panic.
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

volatile int panicked = 0;

// lock to avoid interleaving concurrent printf's.
// pr structure for printf
static struct {
  struct spinlock lock;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

static void printint(long long xx, int base, int sign)
{
  char buf[16];
  int i;
  unsigned long long x;

  // If we are interested in negative numbers, and if x could be a negative number
  // xx is an integer -> a signed integer, it can potentially hold negative and positive numbers
  if(sign && (sign = (xx < 0)))
    // Set x to negative
    x = -xx;
  else
    x = xx; // -1 -> 0xffffffff in twos complement

  i = 0;
  // Fill up a buffer -> array of characters -> 0 to 15 in hex
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  // Iterating backwards, then printng out by character
  while(--i >= 0)
    consputc(buf[i]);
}

// Example program for printing integer
// int y = 1234;
// printf("%d", y);
// xx = 1234
// x = 1234
// // Converts a number into its string representation of an number, modulo indexes directly into the initial dictionary
// // Iterating backwards to put into buffer
// buf = {"4", "3", "2", "1"}


// Example progrma for printing an address
// size of unint64 = 8, 8 x 2 = 16 - total amount of times to iterate over 4 bits at a time
// 0x123456789abdef0
// x >> (sizeof(uint64) * 8 - 4)
// Take x, shift it down by 60 bits
// At every iteration, shift left by 4 bits
static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console.
// Variatic function - takes an optional # of arguments, char* pointer called format
int printf(char *fmt, ...) {
  va_list ap; // Variatic argument list - argument pointer
  int i, cx, c0, c1, c2, locking;
  char *s;

  // Locking procedure
  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);


  va_start(ap, fmt);
  // Make it into a byte, to evaluate it into a value either 0 or 1 i.e we did not reach the end of the string
  for(i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
    // If it is anything else but the percentage symbol. put a char on the console
    if(cx != '%'){
      consputc(cx);
      continue;
    }
    // Increment the index if we encountered a percent symbol
    i++;
    // Assign to the next char in the screen
    c0 = fmt[i+0] & 0xff;
    c1 = c2 = 0;

    if(c0) c1 = fmt[i+1] & 0xff;
    if(c1) c2 = fmt[i+2] & 0xff;

    // Different types of possible arguments that can be passed in
    if(c0 == 'd'){
      printint(va_arg(ap, int), 10, 1);
    } else if(c0 == 'l' && c1 == 'd'){
      printint(va_arg(ap, uint64), 10, 1);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'd'){
      printint(va_arg(ap, uint64), 10, 1);
      i += 2;
    } else if(c0 == 'u'){
      printint(va_arg(ap, int), 10, 0);
    } else if(c0 == 'l' && c1 == 'u'){
      printint(va_arg(ap, uint64), 10, 0);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'u'){
      printint(va_arg(ap, uint64), 10, 0);
      i += 2;
    } else if(c0 == 'x'){
      printint(va_arg(ap, int), 16, 0);
    } else if(c0 == 'l' && c1 == 'x'){
      printint(va_arg(ap, uint64), 16, 0);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'x'){
      printint(va_arg(ap, uint64), 16, 0);
      i += 2;
    } else if(c0 == 'p'){
      printptr(va_arg(ap, uint64));
    } else if(c0 == 's'){
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
    } else if(c0 == '%'){
      consputc('%');
    } else if(c0 == 0){
      break;
    } else {
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c0);
    }

#if 0
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
#endif
  }
  va_end(ap);
  if(locking)
    release(&pr.lock);

  return 0;
}

void panic(char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf("%s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  // Infinite loop crashes program
  for(;;)
    ;
}

void printfinit(void)
{
  initlock(&pr.lock, "pr"); // Start spinlock
  pr.locking = 1; 
}
