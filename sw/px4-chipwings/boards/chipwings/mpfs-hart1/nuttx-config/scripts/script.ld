/****************************************************************************
 * Chipwings Phase 5: PX4 on U54 hart 1 next to Linux on harts 2-4.
 *
 * Same sections as sw/configs/nsh-rpmsg-hart1/ld.script (Phase 4). PX4 is too
 * big for the old 2 MiB + 1 MiB slot, so it moves above the RPMsg buffers,
 * still inside the 32 MiB the Linux device tree reserves (no-map) for hart 1:
 *
 *   0xA2000000  3 MiB    unused (the Phase 4 NSH image)
 *   0xA2300000  4 KiB    RPMsg resource table (sw/patches/0002); the Linux
 *                        device tree points miv_remoteproc's "rsc-table" here
 *   0xA2400000  64 KiB   vring0, vring1 (CONFIG_MPFS_CH1_VRING*_ADDR)
 *   0xA2410000  256 KiB  RPMsg buffers (Linux DT vdev0buffer)
 *   0xA2450000  12 MiB   code + rodata + .data load image
 *   0xA3050000  ~16 MiB  .data/.bss + heap (CONFIG_RAM_START/CONFIG_RAM_SIZE)
 *   0xA4040000           end of the Linux reservation
 ****************************************************************************/

MEMORY
{
    progmem (rx) : ORIGIN = 0xa2450000, LENGTH = 12M   /* w/ cache */
    sram   (rwx) : ORIGIN = 0xa3050000, LENGTH = 0xff0000   /* w/ cache */
    rsctbl (rw)  : ORIGIN = 0xa2300000, LENGTH = 4K
}

OUTPUT_ARCH("riscv")

__ksram_start = ORIGIN(sram);
__ksram_size = LENGTH(sram);
__ksram_end = ORIGIN(sram) + LENGTH(sram);

/* Explicit segments: .init_section holds the C++ constructor table, which the
 * compiler marks writable; left to itself ld would merge it into the code
 * segment and make that RWX. The table is only read at startup.
 */

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    data   PT_LOAD FLAGS(6);   /* R W */
    bss    PT_LOAD FLAGS(6);   /* R W */
    rsctbl PT_LOAD FLAGS(6);   /* R W */
}

ENTRY(_stext)
EXTERN(__start)
SECTIONS
{
    .text : {
        _stext = ABSOLUTE(.);
        *(.start .start.*)
        *(.text .text.*)
        *(.fixup)
        *(.gnu.warning)
        *(.rodata .rodata.* .srodata .srodata.*)
        *(.gnu.linkonce.t.*)
        *(.glue_7)
        *(.glue_7t)
        *(.got)
        *(.gcc_except_table)
        *(.gnu.linkonce.r.*)
        _etext = ABSOLUTE(.);
    } > progmem :text

    .init_section : ALIGN(4) {
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
    } > progmem :text

    _eronly = ABSOLUTE(.);

    .data : ALIGN(4) {
        _sdata = ABSOLUTE(.);
        *(.data .data.*)
        *(.sdata .sdata.* .sdata2.*)
        *(.gnu.linkonce.d.*)
        *(.gnu.linkonce.s.*)
        CONSTRUCTORS
        . = ALIGN(4);
        _edata = ABSOLUTE(.);
    } > sram AT > progmem :data

    /* Loaded as zeros with the image; NuttX fills it in at rptun start */

    .resource_table : ALIGN(8) {
        KEEP(*(.resource_table))
    } > rsctbl :rsctbl

    PROVIDE(__global_pointer$ = _sdata + ((_edata - _sdata) / 2));

    .bss : ALIGN(4) {
        _sbss = ABSOLUTE(.);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(.gnu.linkonce.b.*)
        *(.gnu.linkonce.sb.*)
        *(COMMON)
        . = ALIGN(32);
        _ebss = ABSOLUTE(.);
    } > sram :bss

    /* Stabs debugging sections. */

    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .stab.excl 0 : { *(.stab.excl) }
    .stab.exclstr 0 : { *(.stab.exclstr) }
    .stab.index 0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment 0 : { *(.comment) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
}
