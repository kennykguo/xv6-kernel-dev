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

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w; // write next to uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // read next from uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

extern volatile int panicked; // from printf.c

void uartstart();

void uartinit(void) {

  // disable interrupts
  // write reg -> disable interrupts for the chip, not the CPU
  // write to memory address that represents a piece of hardware
  WRITE_UART_REG(IER, 0x00);

  // special mode to set baud rate.
  // Special value, to change driver behaviour -> line control register
  // UART is a protocol that wiggles electrical lines up and down. Clock signal is used to signal that data is a certain state. UART has no clock, both sides agree upon fixed time scale - baud rate
  WRITE_UART_REG(LCR, LCR_BAUD_LATCH);
  // LSB for baud rate of 38.4K.
  WRITE_UART_REG(0, 0x03);
  // MSB for baud rate of 38.4K.
  WRITE_UART_REG(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity. Transferred 8 bits at a time, through UART
  WRITE_UART_REG(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs. FIFO Queues DS for data
  WRITE_UART_REG(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts. Delivered by device to OS
  WRITE_UART_REG(IER, IER_TX_ENABLE | IER_RX_ENABLE); 

  initlock(&uart_tx_lock, "uart"); // Spinlock - only one CPU can interact with UART
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void
uartputc(int c)
{
  acquire(&uart_tx_lock);

  if(panicked){
    for(;;)
      ;
  }
  while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
    // buffer is full.
    // wait for uartstart() to open up space in the buffer.
    sleep(&uart_tx_r, &uart_tx_lock);
  }
  uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
  uart_tx_w += 1;
  uartstart();
  release(&uart_tx_lock);
}


// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void uartputc_sync(int c)
{
  push_off(); // Disable the interrupts

  if(panicked){
    for(;;)
      ;
  }

  // wait for Transmit Holding Empty to be set in LSR.
  // Wait for line status register to set to empty, masking bit for transfer, spin in while loop
  while((READ_UART_OFFSET_REG(LSR) & LSR_TX_IDLE) == 0)
    ;
  // Transmit holding register
  WRITE_UART_REG(THR, c);

  pop_off(); // Pop off interrupt stack
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void uartstart()
{
  while(1){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      READ_UART_OFFSET_REG(ISR);
      return;
    }
    
    if((READ_UART_OFFSET_REG(LSR) & LSR_TX_IDLE) == 0){
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      return;
    }
    
    int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
    uart_tx_r += 1;
    
    // maybe uartputc() is waiting for space in the buffer.
    wakeup(&uart_tx_r);
    
    WRITE_UART_REG(THR, c);
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
    consoleintr(c);
  }

  // send buffered characters.
  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);
}
