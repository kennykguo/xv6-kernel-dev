//
// low-level driver routines for 16550a UART.
// 16550a 0 is a chip - Memory mapped design with registers for control
// Drivers can be programmed
// XV6 is designed for a virtual computer, not a physical one
// qemu is an emulator - Computer has virtual hardware associated iwth it
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h" 

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
// Takes base UART addr, and then add reg, then casting to a pointer a memory address -> just a single byte
// No alignment trouble
// The keyboard volatile -> tells the compiler to not optimize code utilizing this variable
#define UART_OFFSET(reg) ((volatile unsigned char *)(UART0 + (reg)))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

#define READ_UART_OFFSET_REG(reg) (*(UART_OFFSET(reg)))
#define WRITE_UART_REG(reg, v) (*(UART_OFFSET(reg)) = (v)) // Pointer dereference function to translate value into memory address, then write

// uart transmit buffer management
// circular buffer system for asynchronous uart transmission
struct spinlock uart_transmit_buffer_lock;

#define UART_TRANSMIT_BUFFER_SIZE 32
char uart_transmit_buffer[UART_TRANSMIT_BUFFER_SIZE];
uint64 uart_transmit_write_index; // index where next character will be written
uint64 uart_transmit_read_index;  // index of next character to be sent to uart

extern volatile int panicked; // from printf.c

void uart_start();

// initialize the 16550a universal asynchronous receiver/transmitter (uart) chip
// this function configures the uart hardware registers to enable serial communication
// the uart provides the low-level interface for console input/output
// x
void uart_init(void) {
  
  // step 1 - disable all uart interrupts during initialization
  // interrupt enable register - ier - controls which uart events generate interrupts
  // writing 0x00 disables all interrupt sources
  // this prevents spurious interrupts while we configure other registers
  const uint8 disable_all_interrupts = 0x00;
  WRITE_UART_REG(IER, disable_all_interrupts);

  // step 2 - baud rate configuration mode
  // line control register - lcr - controls data format and special modes
  // setting the baud latch bit (bit 7) switches uart into baud rate setting mode
  // in this mode, registers 0 and 1 become baud rate divisor instead of data registers
  // uart communication requires both sides to agree on the same baud rate (bits per second)
  WRITE_UART_REG(LCR, LCR_BAUD_LATCH);
  
  // step 3 - configure baud rate to 38400 bits per second
  // baud rate = uart_clock_frequency / (16 * divisor)
  // for qemu's emulated uart: 1.8432 MHz / (16 * 3) = 38400 baud
  // divisor = 3, so we write 0x03 to low byte and 0x00 to high byte
  const uint8 baud_rate_divisor_low_byte = 0x03;   // least significant byte of divisor
  const uint8 baud_rate_divisor_high_byte = 0x00;  // most significant byte of divisor
  WRITE_UART_REG(0, baud_rate_divisor_low_byte);   // divisor latch low register
  WRITE_UART_REG(1, baud_rate_divisor_high_byte);  // divisor latch high register

  // step 4 - exit baud rate mode and configure data format
  // set line control register for normal operation
  // - clear baud latch bit (exit baud rate mode)
  // - set 8 data bits per character (standard for text)
  // - set 1 stop bit (standard)
  // - no parity bit (no error checking)
  WRITE_UART_REG(LCR, LCR_EIGHT_BITS);

  // step 5 - enable and configure fifo (first in, first out) buffers
  // fifo control register (fcr) manages internal transmit and receive buffers
  // fifos improve performance by buffering multiple characters
  // fcr_fifo_enable - turn on 16-byte transmit and receive fifos
  // fcr_fifo_clear - clear any existing data in both fifos
  WRITE_UART_REG(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // step 6 - enable interrupts for transmit and receive operations
  // interrupt enable register controls which uart events will interrupt the cpu
  // ier_tx_enable: interrupt when transmit holding register becomes empty (ready for next char)
  // ier_rx_enable: interrupt when receive holding register has data (character received)
  // these interrupts allow the os to efficiently handle serial i/o without polling
  WRITE_UART_REG(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  // step 7 - initialize synchronization lock for uart transmit buffer
  // the uart transmit buffer is shared between different kernel threads
  // this spinlock ensures only one cpu can modify the transmit buffer at a time
  // prevents race conditions when multiple cpus try to print simultaneously
  create_lock(&uart_transmit_buffer_lock, "uart_transmit_buffer_lock");
}



// queue character for uart transmission using buffered output
// blocks if transmit buffer is full, waiting for space to become available
// safe to call from any context except interrupt handlers (due to blocking)
// character_to_queue - ascii character to send to console
void uart_put_char(int character_to_queue)
{
  acquire(&uart_transmit_buffer_lock);

  // special case - if system has panicked, spin forever without doing anything
  // this prevents further corruption and ensures panic messages are visible
  if(panicked){
    for(;;)
      ;  // infinite loop - system is dead
  }
  
  // wait for space in circular transmit buffer
  // condition true when buffer is completely full (write caught up to read + buffer size)
  while(uart_transmit_write_index == uart_transmit_read_index + UART_TRANSMIT_BUFFER_SIZE){
    // transmit buffer full - sleep until uart_start() creates space
    // sleeping releases the lock temporarily and wakes up when space available
    sleep(&uart_transmit_read_index, &uart_transmit_buffer_lock);
  }
  
  // insert character into circular buffer at current write position
  // modulo operation handles wraparound when write index exceeds buffer size
  uint write_position = uart_transmit_write_index % UART_TRANSMIT_BUFFER_SIZE;
  uart_transmit_buffer[write_position] = character_to_queue;
  
  // advance write index to next available slot
  uart_transmit_write_index += 1;
  
  // attempt to start transmission if uart hardware is idle
  uart_start();
  
  release(&uart_transmit_buffer_lock);
}

// synchronous uart character output - used by printf and panic
// this function directly writes to uart hardware without using interrupts or buffers
// it blocks (spins) until the uart is ready, making it safe for printf and error handling
// critical function: this is what actually sends characters to the console during boot
void uartputc_sync(int character_to_transmit)
{
  // disable interrupts on this cpu to prevent race conditions
  // push_off() increments interrupt disable count and saves current interrupt state
  // this prevents the scheduler from interrupting us while we're writing to uart
  push_off(); 

  // if kernel has panicked, loop forever to halt the cpu
  // panicked is set by panic() function when a fatal error occurs
  // this prevents further kernel execution that might corrupt state
  if(panicked){
    for(;;)
      ;
  }

  // busy-wait (spin) until uart transmit holding register is empty
  // line status register (lsr) contains uart status flags
  // lsr_tx_idle bit is set when transmit holding register can accept a new character
  // we mask the lsr value with lsr_tx_idle and loop until this bit becomes 1
  // this polling approach is simpler than interrupts for printf/panic scenarios
  uint8 line_status_register_value;
  do {
    line_status_register_value = READ_UART_OFFSET_REG(LSR);
  } while((line_status_register_value & LSR_TX_IDLE) == 0);

  // transmit holding register is now empty, safe to write the character
  // writing to thr (transmit holding register) queues the character for transmission
  // the uart hardware will automatically send this character over the serial line
  WRITE_UART_REG(THR, character_to_transmit);

  // restore previous interrupt state on this cpu
  // pop_off() decrements interrupt disable count and restores interrupts if count reaches 0
  // this re-enables interrupts if they were enabled when we called push_off()
  pop_off();
}

// initiate uart transmission by draining buffered characters to hardware
// processes all available characters in transmit buffer until empty or hardware busy
// caller must hold uart_transmit_buffer_lock to ensure thread safety
// called from uart_put_char() (top half) and uart interrupt handler (bottom half)
void uart_start()
{
  while(1){
    // check if transmit buffer is empty (no pending characters to send)
    if(uart_transmit_write_index == uart_transmit_read_index){
      // buffer empty - clear any pending interrupt status and exit
      READ_UART_OFFSET_REG(ISR);  // reading interrupt status register clears pending interrupts
      return;
    }
    
    // check if uart hardware transmit holding register can accept another byte
    uint64 line_status_register = READ_UART_OFFSET_REG(LSR);
    if((line_status_register & LSR_TX_IDLE) == 0){
      // uart transmit holding register is busy - cannot accept more data right now
      // hardware will generate interrupt when ready to accept next byte
      return;
    }
    
    // extract next character from circular transmit buffer
    uint read_position = uart_transmit_read_index % UART_TRANSMIT_BUFFER_SIZE;
    int character_to_transmit = uart_transmit_buffer[read_position];
    uart_transmit_read_index++;  // advance read pointer to next character position
    
    // wake up any threads waiting for buffer space (uart_put_char may be sleeping)
    wake_up(&uart_transmit_read_index);
    
    // send character to uart hardware transmit holding register
    WRITE_UART_REG(THR, character_to_transmit);
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  if(READ_UART_OFFSET_REG(LSR) & 0x01){
    // input data is ready.
    return READ_UART_OFFSET_REG(RHR);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from devintr().
void
uartintr(void)
{
  // read and process incoming characters.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    console_intr(c);
  }

  // send buffered characters.
  acquire(&uart_transmit_buffer_lock);
  uart_start();
  release(&uart_transmit_buffer_lock);
}
