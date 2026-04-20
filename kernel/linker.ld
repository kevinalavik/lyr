OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(lyr_entry)

PHDRS
{
	text PT_LOAD FLAGS((1 << 0) | (1 << 2));
	rodata PT_LOAD FLAGS(1 << 2);
	data PT_LOAD FLAGS((1 << 1) | (1 << 2));
}

SECTIONS
{
	. = 0xffffffff80000000;

	_start_text = .;

	.text : {
		*(.text .text.*)
	} :text

	_end_text = .;

	. = ALIGN(CONSTANT(MAXPAGESIZE));

	_start_rodata = .;

	.rodata : {
		*(.rodata .rodata.*)
		. = ALIGN(16);
		__axapi_exports_start = .;
		KEEP(*(.axapi.exports))
		__axapi_exports_end = .;
	} :rodata

	_end_rodata = .;

	. = ALIGN(CONSTANT(MAXPAGESIZE));

	_start_data = .;

	.data : {
		*(.data .data.*)
	} :data

	.bss : {
		*(COMMON)
		*(.bss .bss.*)
	} :data

	_end_data = .;

	/DISCARD/ : {
		*(.eh_frame)
		*(.note .note.*)
	}
}