/*--------------------------------------------------------------------------------
	This Source Code Form is subject to the terms of the Mozilla Public License,
	v. 2.0. If a copy of the MPL was not distributed with this file, You can
	obtain one at https://mozilla.org/MPL/2.0/.
--------------------------------------------------------------------------------*/
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(armv6k)
ENTRY(_start)

PHDRS
{
	code   PT_LOAD FLAGS(5) /* Read | Execute */;
	rodata PT_LOAD FLAGS(4) /* Read */;
	data   PT_LOAD FLAGS(6) /* Read | Write */;
}

SECTIONS
{
	PROVIDE(__start__ = 0x100000);
	. = __start__;

	.text ALIGN(0x1000) :
	{
		KEEP( *(.crt0) )
		KEEP( *(.init) )
		. = ALIGN(4);
		*(.text)
		*(.text.*)
		*(.glue_7)
		*(.glue_7t)
		*(.stub)
		*(.gnu.warning)
		*(.gnu.linkonce.t*)
		. = ALIGN(4);
		KEEP( *(.fini) )
		. = ALIGN(4);
	} : code

	. = ALIGN(0x1000);
	.rodata : { *(.rodata) *(.rodata.*) . = ALIGN(4); } : rodata
	. = ALIGN(0x1000);
	.data : { *(.data) *(.data.*) *(.got) *(.got.plt) . = ALIGN(4); *(.tdata) *(.tbss) } : data
	.bss : { *(.bss) *(.bss.*) *(COMMON) } : data
	.tls : { *(.tls) *(.tls.*) } : data
	/DISCARD/ : { *(.ARM.exidx) *(.ARM.exidx.*) *(.ARM.extab) *(.ARM.extab.*) *(.eh_frame) *(.eh_frame.*) *(.gcc_except_table) *(.gnu.linkonce.e*) }
}