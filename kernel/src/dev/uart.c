#include <dev/uart.h>
#include <cpu/instr.h>

/* helpers */
void _uart_write_reg(uint16_t port, uint8_t offset, uint8_t value)
{
	outb(port + offset, value);
}

uint8_t _uart_read_reg(uint16_t port, uint8_t offset)
{
	return inb(port + offset);
}

void _uart_wait_for_transmitter_empty(uint16_t port)
{
	uart_lsr_t lsr = { 0 };

	do {
		uint8_t raw = _uart_read_reg(port, UART_REG_LSR);
		lsr = *(uart_lsr_t *)&raw;
	} while (!lsr.transmitter_holding_register_empty);
}

/* main API */
int uart_init()
{
	uint16_t port = DEFAULT_UART_PORT;
	uint16_t divisor = 115200 / DEFAULT_UART_BAUD_RATE;

	/*
    main COM setup:
        - no interrupts
        - 8 data bits
        - 1 stop bit
        - no parity
        - Enable FIFO, clear them, with 14-byte threshold
        - IRQs enabled, clear them, with 14-byte threshold
        - loopback mode and test
        - normal operation
    */

	/* disable all interrupts */
	uart_ier_t ier = { 0 };
	_uart_write_reg(port, UART_REG_IER, *((uint8_t *)&ier));

	/* enable DLAB */
	uart_lcr_t lcr = { 0 };
	lcr.dlab = 1;
	_uart_write_reg(port, UART_REG_LCR, *((uint8_t *)&lcr));

	/* set baud rate divisor */
	_uart_write_reg(port, UART_REG_DLL, divisor & 0xFF);
	_uart_write_reg(port, UART_REG_DLM, (divisor >> 8) & 0xFF);

	/* disable DLAB and set data bits, stop bits and parity */
	lcr.dlab = 0;
	lcr.db = UART_DATA_BITS_8; // 8 data bits
	lcr.sb = UART_STOP_BITS_1; // 1 stop bit
	lcr.pb = UART_PARITY_NONE; // no parity
	_uart_write_reg(port, UART_REG_LCR, *((uint8_t *)&lcr));

	/* enable FIFO, clear them, with 14-byte threshold */
	uart_fcr_t fcr = { 0 };
	fcr.enable_fifo = 1;
	fcr.clear_receive_fifo = 1;
	fcr.clear_transmit_fifo = 1;
	_uart_write_reg(port, UART_REG_FCR,
					*((uint8_t *)&fcr) | UART_FCR_TRIGGER_LEVEL_14);

	/* enable IRQs, clear them, with 14-byte threshold */
	uart_ier_t ier_enable = { 0 };
	ier_enable.received_data_available = 1;
	ier_enable.transmitter_holding_register_empty = 1;
	_uart_write_reg(port, UART_REG_IER, *((uint8_t *)&ier_enable));

	/* enable loopback mode */
	uart_mcr_t mcr = { 0 };
	mcr.loop = 1;
	mcr.out2 = 1;
	_uart_write_reg(port, UART_REG_MCR, *((uint8_t *)&mcr));

	/* test loopback mode */
	_uart_write_reg(port, UART_REG_THR, 0xAE);
	if (_uart_read_reg(port, UART_REG_RBR) != 0xAE)
		return -1;

	/* disable loopback mode and set normal operation */
	mcr.loop = 0;
	mcr.out2 = 1;
	_uart_write_reg(port, UART_REG_MCR, *((uint8_t *)&mcr));

	return 0;
}

void uart_wbuf(const char *buf, size_t len)
{
	uint16_t port = DEFAULT_UART_PORT;

	for (size_t i = 0; i < len; i++) {
		_uart_wait_for_transmitter_empty(port);
		_uart_write_reg(port, UART_REG_THR, buf[i]);
	}
}

void uart_wstr(const char *str)
{
	uint16_t port = DEFAULT_UART_PORT;

	for (size_t i = 0; str[i] != '\0'; i++) {
		_uart_wait_for_transmitter_empty(port);
		_uart_write_reg(port, UART_REG_THR, str[i]);
	}
}

void uart_wch(char c)
{
	uint16_t port = DEFAULT_UART_PORT;

	_uart_wait_for_transmitter_empty(port);
	_uart_write_reg(port, UART_REG_THR, c);
}