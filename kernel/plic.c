#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// risc-v platform level interrupt controller (plic) driver
//
// the plic is responsible for routing external device interrupts to cpu cores
// it acts as a centralized interrupt distribution hub that:
// 1. receives interrupt signals from devices (uart, disk, network, etc.)
// 2. assigns priority levels to different interrupt sources  
// 3. delivers interrupts to specific cpu cores based on configuration
// 4. provides claim/complete mechanism for interrupt acknowledgment
//
// this allows the operating system to handle device i/o efficiently
// without constantly polling device status registers

// initialize global plic settings that apply to all cpu cores
// called once during kernel boot to configure interrupt priorities
// must be called before any individual cpu cores initialize their plic settings
// x
void configure_global_interrupt_priorities(void) {
  // configure interrupt priority levels for each device
  // priority must be non-zero to enable the interrupt source (zero = disabled)
  // higher priority numbers get delivered first when multiple interrupts are pending
  
  // emulate devices in qemu as mmapped reg
  // uart interrupt priority: set to 1 (lowest non-zero priority)
  // uart generates interrupts when characters are received or transmit buffer is empty
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
  
  // virtio disk interrupt priority: set to 1 (same priority as uart)
  // disk generates interrupts when read/write operations complete
  *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1;
}

// initialize plic settings for a specific cpu core (hart)
// called on each cpu core during initialization to enable interrupt delivery
// configures which interrupts this cpu will receive and at what threshold
void enable_interrupts_for_this_cpu(void)
{
  int current_cpu_hart_id = cpuid();
  
  // enable specific interrupt sources for this cpu's supervisor mode
  // each bit in the enable register corresponds to an interrupt source
  // setting bit N to 1 means this cpu can receive interrupt N
  *(uint32*)PLIC_SENABLE(current_cpu_hart_id) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);
  
  // set interrupt priority threshold for this cpu's supervisor mode
  // only interrupts with priority > threshold will be delivered to this cpu
  // setting to 0 means all enabled interrupts (priority >= 1) will be delivered
  *(uint32*)PLIC_SPRIORITY(current_cpu_hart_id) = 0;
}

// claim the next pending interrupt for processing
// this function is called by the interrupt handler to determine which device needs service
// returns the interrupt request number, or 0 if no interrupts are pending
int
plic_claim(void)
{
  int current_cpu_hart_id = cpuid();
  
  // read from plic claim register to get next highest-priority pending interrupt
  // this operation atomically claims the interrupt for this cpu
  // other cpus cannot claim the same interrupt until we complete it
  int interrupt_request_number = *(uint32*)PLIC_SCLAIM(current_cpu_hart_id);
  return interrupt_request_number;
}

// signal completion of interrupt processing
// this function must be called after servicing an interrupt to allow plic to deliver more
// tells the plic that we're done handling this interrupt source
void
plic_complete(int interrupt_request_number)
{
  int current_cpu_hart_id = cpuid();
  
  // write interrupt number back to claim register to signal completion
  // this releases the interrupt and allows plic to deliver new interrupts
  // the same register is used for both claim (read) and complete (write) operations
  *(uint32*)PLIC_SCLAIM(current_cpu_hart_id) = interrupt_request_number;
}
