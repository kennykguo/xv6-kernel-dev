/* A linker script is essentially a set of instructions that 
tells the linker how to organize the different sections of your
compiled code into the final executable binary. Let me explain 
how it works in the context of xv6. */

OUTPUT_ARCH( "riscv" ) /* Output should be for the RISCV architecture */
ENTRY( _entry ) /* Sets up the entry */
/* A linker script specifies:
The memory layout of the final executable
Where each section should be placed in memory
The entry point (where execution begins)
Special symbols that your code can reference */

SECTIONS
{
  /*
   * ensure that entry.S / _entry is at 0x80000000,
   * where qemu's -kernel jumps.
   */
   /* QEMU jumps here when it loads the kernel */
  . = 0x80000000;

  /* Places all .text, .rodata, .data, .bss, parts of object files into this section of the output - also aligns trampoline to a page boundary
  Ensures kernel also fits into one page  */
  .text : {
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    PROVIDE(etext = .);
  }

  /* Read only data  */
  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  }

  /* End of all sections */
  PROVIDE(kernel_binary_end = .); /* PROVIDED AS symbol */
}
