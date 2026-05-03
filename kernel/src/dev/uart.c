#include <dev/uart.h>
#include <dev/async.h>
#include <sync/spinlock.h>
#include <stdbool.h>

#define UART_TX_Q_SIZE 8192
#define UART_DRAIN_BUDGET 128

static spinlock_t uart_lock = SPINLOCK_INIT;
static char uart_tx_q[UART_TX_Q_SIZE];
static size_t uart_tx_rpos;
static size_t uart_tx_wpos;
static size_t uart_tx_dropped;

static bool _uart_tx_empty(void)
{
	return uart_tx_rpos == uart_tx_wpos;
}

static bool _uart_tx_full(void)
{
	return ((uart_tx_wpos + 1) % UART_TX_Q_SIZE) == uart_tx_rpos;
}

static void _uart_write_reg(uint16_t port, uint8_t offset, uint8_t value)
{
	async_io_outb_sync(port + offset, value);
}

static uint8_t _uart_read_reg(uint16_t port, uint8_t offset)
{
	return async_io_inb_sync(port + offset);
}

static bool _uart_transmitter_empty(uint16_t port)
{
	uart_lsr_t lsr = { 0 };
	uint8_t raw = _uart_read_reg(port, UART_REG_LSR);
	lsr = *(uart_lsr_t *)&raw;
	return lsr.transmitter_holding_register_empty;
}

static void _uart_enqueue_locked(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (_uart_tx_full()) {
			uart_tx_dropped += len - i;
			break;
		}

		uart_tx_q[uart_tx_wpos] = buf[i];
		uart_tx_wpos = (uart_tx_wpos + 1) % UART_TX_Q_SIZE;
	}
}

static size_t _uart_drain_locked(uint16_t port, size_t budget)
{
	size_t drained = 0;

	while (!_uart_tx_empty() && (budget == 0 || drained < budget)) {
		if (!_uart_transmitter_empty(port))
			break;

		_uart_write_reg(port, UART_REG_THR, uart_tx_q[uart_tx_rpos]);
		uart_tx_rpos = (uart_tx_rpos + 1) % UART_TX_Q_SIZE;
		drained++;
	}

	return drained;
}

static size_t uart_async_drain(size_t budget, void *context)
{
	(void)context;
	return uart_drain(budget);
}

int uart_init(void)
{
	uint16_t port = DEFAULT_UART_PORT;
	uint16_t divisor = 115200 / DEFAULT_UART_BAUD_RATE;

	uart_ier_t ier = { 0 };
	_uart_write_reg(port, UART_REG_IER, *((uint8_t *)&ier));

	uart_lcr_t lcr = { 0 };
	lcr.dlab = 1;
	_uart_write_reg(port, UART_REG_LCR, *((uint8_t *)&lcr));

	_uart_write_reg(port, UART_REG_DLL, divisor & 0xFF);
	_uart_write_reg(port, UART_REG_DLM, (divisor >> 8) & 0xFF);

	lcr.dlab = 0;
	lcr.db = UART_DATA_BITS_8;
	lcr.sb = UART_STOP_BITS_1;
	lcr.pb = UART_PARITY_NONE;
	_uart_write_reg(port, UART_REG_LCR, *((uint8_t *)&lcr));

	uart_fcr_t fcr = { 0 };
	fcr.enable_fifo = 1;
	fcr.clear_receive_fifo = 1;
	fcr.clear_transmit_fifo = 1;
	_uart_write_reg(port, UART_REG_FCR,
					*((uint8_t *)&fcr) | UART_FCR_TRIGGER_LEVEL_14);

	uart_ier_t ier_enable = { 0 };
	ier_enable.received_data_available = 1;
	ier_enable.transmitter_holding_register_empty = 1;
	_uart_write_reg(port, UART_REG_IER, *((uint8_t *)&ier_enable));

	uart_mcr_t mcr = { 0 };
	mcr.loop = 1;
	mcr.out2 = 1;
	_uart_write_reg(port, UART_REG_MCR, *((uint8_t *)&mcr));
	_uart_write_reg(port, UART_REG_THR, 0xAE);
	if (_uart_read_reg(port, UART_REG_RBR) != 0xAE)
		return -1;

	mcr.loop = 0;
	mcr.out2 = 1;
	_uart_write_reg(port, UART_REG_MCR, *((uint8_t *)&mcr));
	async_io_register_drain_hook(uart_async_drain, NULL);
	return 0;
}

void uart_wbuf(const char *buf, size_t len)
{
	if (!buf || len == 0)
		return;

	spinlock_acquire(&uart_lock);
	_uart_enqueue_locked(buf, len);
	spinlock_release(&uart_lock);
	uart_drain(UART_DRAIN_BUDGET);
}

void uart_wstr(const char *str)
{
	size_t len = 0;
	if (!str)
		return;
	while (str[len] != '\0')
		len++;
	uart_wbuf(str, len);
}

void uart_wch(char c)
{
	uart_wbuf(&c, 1);
}

size_t uart_drain(size_t budget)
{
	size_t drained = 0;

	if (!spinlock_try_acquire(&uart_lock))
		return 0;

	drained = _uart_drain_locked(DEFAULT_UART_PORT, budget);
	spinlock_release(&uart_lock);
	return drained;
}

void uart_flush(void)
{
	for (;;) {
		bool checked_empty = false;
		bool empty = false;
		size_t drained = uart_drain(UART_DRAIN_BUDGET);

		if (spinlock_try_acquire(&uart_lock)) {
			empty = _uart_tx_empty();
			checked_empty = true;
			spinlock_release(&uart_lock);
		}

		if (checked_empty && empty)
			break;

		if (drained == 0)
			break;
	}
}

size_t uart_dropped_bytes(void)
{
	size_t dropped = 0;

	if (spinlock_try_acquire(&uart_lock)) {
		dropped = uart_tx_dropped;
		spinlock_release(&uart_lock);
	}

	return dropped;
}
