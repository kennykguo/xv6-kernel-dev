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
} printf_lock;

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

// formatted print to console
// variadic function that accepts format string and variable arguments (like standard printf)
// this is the main entry point for all kernel print statements including boot messages
// the ... is the variadic arguments syntax in C. It allows the function to accept a variable number of arguments after the required format_string parameter.
// the function uses va_list, va_start(), va_arg(), and va_end() macros to access these variable arguments at runtime.
int printf(char *format_string, ...) {
  va_list argument_pointer; // variadic argument list for accessing variable arguments - defined in __stdarg_va_list.h
  int format_index, // - tracks current position in format string during parsing
      current_character, // - holds current character being processed from format string
      format_specifier_char, // - character immediately after % in format specifiers
      next_char, // - second character after % for multi-char specifiers like %ld
      third_char, // - third character after % for three-char specifiers like %lld
      should_use_locking; // - flag to determine if printf locking is enabled
  
  char *string_argument;

  // determine if we should use locking to prevent interleaved output from multiple cpus
  // printf_lock.locking is enabled after printf_init() to synchronize printf calls
  should_use_locking = printf_lock.locking;
  if(should_use_locking)
    acquire(&printf_lock.lock);

  // initialize variadic argument processing
  // va_start sets up argument_pointer to access arguments after format_string
  va_start(argument_pointer, format_string);
  
  // main formatting loop - process each character in the format string
  // walk through format string character by character until null terminator
  for(format_index = 0; (current_character = format_string[format_index] & 0xff) != 0; format_index++) {
    
    // handle regular characters (non-format specifiers)
    // if character is not '%', just output it directly to console
    if(current_character != '%'){
      consputc(current_character);
      continue;
    }
    
    // found '%' - this starts a format specifier
    // advance past the '%' and examine the format specifier character(s)
    format_index++;
    next_char = third_char = 0;
    // look ahead to handle multi-character format specifiers like %ld, %lld
    format_specifier_char = format_string[format_index + 0] & 0xff;
    if(format_specifier_char) {
      next_char = format_string[format_index + 1] & 0xff;
    }
    if(next_char) {
      third_char = format_string[format_index + 2] & 0xff;
    }

    // format specifier parsing and argument processing
    // checked in order of fewest characters to lowest characters - this logic will always correctly parse the correct formatter
    // handle different format types - %d (decimal), %x (hex), %s (string), %p (pointer), etc.
    // %d - signed decimal integer
    if(format_specifier_char == 'd'){
      printint(va_arg(argument_pointer, int), 10, 1);
    } 
    else if(format_specifier_char == 'l' && next_char == 'd'){
      // %ld - long signed decimal integer (64-bit)
      printint(va_arg(argument_pointer, uint64), 10, 1);
      format_index += 1; // skip over the 'd' since we processed "ld"
    } 
    else if(format_specifier_char == 'l' && next_char == 'l' && third_char == 'd'){
      // %lld - long long signed decimal integer (64-bit)
      printint(va_arg(argument_pointer, uint64), 10, 1);
      format_index += 2; // skip over "ld" since we processed "lld"
    } 
    else if(format_specifier_char == 'u'){
      // %u - unsigned decimal integer
      printint(va_arg(argument_pointer, int), 10, 0);
    } 
    else if(format_specifier_char == 'l' && next_char == 'u'){
      // %lu - long unsigned decimal integer (64-bit)
      printint(va_arg(argument_pointer, uint64), 10, 0);
      format_index += 1;
    } 
    else if(format_specifier_char == 'l' && next_char == 'l' && third_char == 'u'){
      // %llu - long long unsigned decimal integer (64-bit)
      printint(va_arg(argument_pointer, uint64), 10, 0);
      format_index += 2;
    } 
    else if(format_specifier_char == 'x'){
      // %x - hexadecimal integer (lowercase)
      printint(va_arg(argument_pointer, int), 16, 0);
    } 
    else if(format_specifier_char == 'l' && next_char == 'x'){
      // %lx - long hexadecimal integer (64-bit)
      printint(va_arg(argument_pointer, uint64), 16, 0);
      format_index += 1;
    } 
    else if(format_specifier_char == 'l' && next_char == 'l' && third_char == 'x'){
      // %llx - long long hexadecimal integer (64-bit)
      printint(va_arg(argument_pointer, uint64), 16, 0);
      format_index += 2;
    } 
    else if(format_specifier_char == 'p'){
      // %p - pointer address (formatted as 0x... in hexadecimal)
      printptr(va_arg(argument_pointer, uint64));
    } 
    // special type of formatter - string
    // %s - null-terminated string
    else if(format_specifier_char == 's'){
      string_argument = va_arg(argument_pointer, char*);
      // IF THE POINTER IS pointing to NULL (do not confuse with an actual null byte) - defined as (void*)0
      // null ptr != null byte
      if(string_argument == 0) {
        string_argument = "(null)"; // handle null pointer gracefully
      }
      // output each character of the string
      // loop until null byte found (memory with value 0)
      for(; *string_argument; string_argument++) {
        consputc(*string_argument);
      }
    }
    else if(format_specifier_char == '%'){
      // %% - literal percent character
      consputc('%');
    }
    else if(format_specifier_char == 0){
      // format string ended with '%' - break out of loop
      break;
    }
    else {
      // unknown format specifier - print it anyways
      consputc('<UNKNOWN CHARACTER- - ->');
      consputc(format_specifier_char);
    }
  
  // clean up variadic argument processing
  va_end(argument_pointer);
  
  // release the printf lock if we acquired it
  if(should_use_locking)
    release(&printf_lock.lock);

  return 0;
}

void panic(char *s) {
  printf_lock.locking = 0;
  printf("panic - ");
  printf("%s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  // Infinite loop crashes program
  for(;;)
    ;
}

// initialize printf subsystem
// called early in main() to set up synchronization for printf
void printf_init(void)
{
  init_lock(&printf_lock.lock, "printf"); // initialize spinlock for printf synchronization
  printf_lock.locking = 1; // ENABLE locking to prevent interleaved output from multiple cpus (DOES NOT LOCK***)
}
