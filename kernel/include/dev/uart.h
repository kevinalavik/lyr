#ifndef _LYR_DEV_UART_H
#define _LYR_DEV_UART_H

// lyr UART (COM) driver
// source: https://wiki.osdev.org/Serial_Ports

#include <stddef.h>
#include <stdint.h>

#define DEFAULT_UART_PORT 0x3F8
#define DEFAULT_UART_BAUD_RATE 115200

// Register offsets

/* 
Off |   DLAB    |   I/O Access  |   Register
------------------------------------------------------------------------------------
+0      0	        Read	        Receive buffer.
+0      0	        Write	        Transmit buffer.
+1      0	        Read/Write	    Interrupt Enable Register.
+0      1	        Read/Write	    With DLAB set to 1, this is the least significant byte of the divisor value for setting the baud rate.
+1      1	        Read/Write	    With DLAB set to 1, this is the most significant byte of the divisor value.
+2      -	        Read	        Interrupt Identification
+2      -	        Write	        FIFO control registers
+3      -	        Read/Write	    Line Control Register. The most significant bit of this register is the DLAB.
+4      -	        Read/Write	    Modem Control Register.
+5      -	        Read	        Line Status Register.
+6      -	        Read	        Modem Status Register.
+7      -	        Read/Write	    Scratch Register.
*/

#define UART_REG_RBR 0
#define UART_REG_THR 0
#define UART_REG_IER 1
#define UART_REG_DLL 0
#define UART_REG_DLM 1
#define UART_REG_IIR 2
#define UART_REG_FCR 2
#define UART_REG_LCR 3
#define UART_REG_MCR 4
#define UART_REG_LSR 5
#define UART_REG_MSR 6
#define UART_REG_SCR 7

// Line control register
/*
The Line Control register sets the general connection parameters.

Bit 7                    |   Bit 6           |   Bits 5-3    |   Bit 2   |  Bits 1-0
-------------------------------------------------------------------------------------
Divisor Latch Access Bit    Break Enable Bit    Parity Bits	    Stop Bits	Data Bits
*/

typedef struct {
	uint8_t db : 2;
	uint8_t sb : 1;
	uint8_t pb : 3;
	uint8_t be : 1;
	uint8_t dlab : 1;
} __attribute__((packed)) uart_lcr_t;

// Data bits
/*
The number of bits in a character is variable. Having fewer bits is, of course, faster, but they store less information. 
If you are only sending ASCII text, you probably only need 7 bits.
Set this value by writing to the two least significant bits of the Line Control Register [PORT + 3].

Bit 1   |   Bit 0   |  Character Length (bits)
----------------------------------------------
0	        0	        5
0	        1	        6
1	        0	        7
1	        1	        8
*/
#define UART_DATA_BITS_5 0
#define UART_DATA_BITS_6 1
#define UART_DATA_BITS_7 2
#define UART_DATA_BITS_8 3

// Stop bits
/*
The serial controller can be configured to send a number of bits after each character of data.
These reliable bits can be used to by the controller to verify that the sending and receiving devices are in phase.
If the character length is specifically 5 bits, the stop bits can only be set to 1 or 1.5. 
For other character lengths, the stop bits can only be set to 1 or 2.
To set the number of stop bits, set bit 2 of the Line Control Register [PORT + 3].

Bit 2   |  Stop bits
---------------------------------------------------
0	        1
1	        1.5 / 2 (depending on character length)
*/

#define UART_STOP_BITS_1 0
#define UART_STOP_BITS_2 1

// Parity bits
/*
The controller can be made to add or expect a parity bit at the end of each character of data transmitted.
With this parity bit, if a single bit of data is inverted by interference, a parity error can be raised. 
The parity type can be NONE, EVEN, ODD, MARK or SPACE.
If parity is set to NONE, no parity bit will be added and none will be expected. 
If one is sent by the transmitter and not expected by the receiver, it will likely cause an error.
If the parity is MARK or SPACE, the parity bit will be expected to be always set to 1 or 0 respectively.
If the parity is set to EVEN or ODD, the controller calculates the accuracy of the parity by adding together the values of all the data bits and the parity bit. 
If the port is set to have EVEN parity, the result must be even. If it is set to have ODD parity, the result must be odd.
To set the port parity, set bits 3, 4 and 5 of the Line Control Register [PORT + 3].


Bit 5   |  Bit 4    |   Bit 3   |   Parity
------------------------------------------
-	        -	        0	        NONE
0	        0	        1	        ODD
0	        1	        1	        EVEN
1	        0	        1	        MARK
1	        1	        1	        SPACE
*/

#define UART_PARITY_NONE 0
#define UART_PARITY_ODD 1
#define UART_PARITY_EVEN 2
#define UART_PARITY_MARK 3
#define UART_PARITY_SPACE 4

// Interrupt enable register
/*
To communicate with a serial port in interrupt mode, the interrupt-enable-register (see table above) must be set correctly. 
To determine which interrupts should be enabled, a value with the following bits (0 = disabled, 1 = enabled) must be written to the interrupt-enable-register:

Bit 7-4     |   Bit 3       |   Bit 2               |   Bit 1                            |   Bit 0
-------------------------------------------------------------------------------------------------------------------
Reserved        Modem Status    Receiver Line Status	Transmitter Holding Register Empty	Received Data Available
*/

typedef struct {
	uint8_t received_data_available : 1;
	uint8_t transmitter_holding_register_empty : 1;
	uint8_t receiver_line_status : 1;
	uint8_t modem_status : 1;
	uint8_t reserved : 4;
} __attribute__((packed)) uart_ier_t;

// First In First Out Control Register
/*
The First In / First Out Control Register (FCR) is for controlling the FIFO buffers. 
Access this register by writing to port offset +2.

Bits 7-6                |   Bits 5-4    |   Bit 3           |   Bit 2               |   Bit 1               |   Bit 0
-----------------------------------------------------------------------------------------------------------------------------
Interrupt Trigger Level	    Reserved	    DMA Mode Select	    Clear Transmit FIFO	    Clear Receive FIFO	    Enable FIFO's
*/

typedef struct {
	uint8_t enable_fifo : 1;
	uint8_t clear_receive_fifo : 1;
	uint8_t clear_transmit_fifo : 1;
	uint8_t dma_mode_select : 1;
	uint8_t reserved : 4;
} __attribute__((packed)) uart_fcr_t;

/* 
Clear Transmit FIFO and Clear Receive FIFO 
------------------------------------------
Bit 2 being set clears the Transmit FIFO buffer while Bit 1 being set clears the Receive FIFO buffer.
Both bits will set themselves back to 0 after they are done being cleared.
*/

/*
Interrupt Trigger Level
-----------------------
The Interrupt Trigger Level is used to configure how much data must be received in the FIFO Receive buffer before triggering a Received Data Available Interrupt.

Bit 7   |   Bit 6   |   Trigger Level
-------------------------------------
0	        0	        1 Byte
0	        1	        4 Bytes
1	        0	        8 Bytes
1	        1	        14 Bytes
*/

#define UART_FCR_TRIGGER_LEVEL_1 0
#define UART_FCR_TRIGGER_LEVEL_4 0x40
#define UART_FCR_TRIGGER_LEVEL_8 0x80
#define UART_FCR_TRIGGER_LEVEL_14 0xC0

// Interrupt Identification Register
/*
The Interrupt Identification Register (IIR) is for identifying pending interrupts.
Access this register by reading from port offset +2.

Bits 7-6            |   Bits 5-4    |   Bit 3                                           |   Bit 2-1         |   Bit 0
--------------------------------------------------------------------------------------------------------------------------------------
FIFO Buffer State	    Reserved	    Timeout Interrupt Pending (UART 16550) or Reserved	Interrupt State	    Interrupt Pending if 0
*/

typedef struct {
	uint8_t interrupt_pending : 1;
	uint8_t interrupt_state : 2;
	uint8_t timeout_interrupt_pending : 1;
	uint8_t reserved : 4;
} __attribute__((packed)) uart_iir_t;

/*
Interrupt State
---------------
After Interrupt Pending is set, the Interrupt State shows the interrupt that has occurred.
They have varying levels of priority, with high-value interrupts handled first, and low-value interrupts being handled last.

Bit 2   |   Bit 1   |   Interrupt                           |   Priority
---------------------------------------------------------------------------
0	        0	        Modem Status	                        4 (Lowest)
0	        1	        Transmitter Holding Register Empty	    3
1	        0	        Received Data Available	                2
1	        1	        Receiver Line Status	                1 (Highest)
*/

#define UART_IIR_INTERRUPT_MODEM_STATUS 0
#define UART_IIR_INTERRUPT_TRANSMITTER_HOLDING_REGISTER_EMPTY 1
#define UART_IIR_INTERRUPT_RECEIVED_DATA_AVAILABLE 2
#define UART_IIR_INTERRUPT_RECEIVER_LINE_STATUS 3

/*
FIFO Buffer State
-----------------

Bit 7   |   Bit 6   |   State
-------------------------------------------------
0	        0	        No FIFO
0	        1	        FIFO Enabled but Unusable
1	        0	        FIFO Enabled
*/

typedef struct {
	uint8_t fifo_enabled : 1;
	uint8_t fifo_usable : 1;
	uint8_t reserved : 6;
} __attribute__((packed)) uart_fifo_state_t;

// Modem Control Register
/*
The Modem Control Register is one half of the hardware handshaking registers.
While most serial devices no longer use hardware handshaking, The lines are still included in all 16550 compatible UARTS.
These can be used as general purpose output ports, or to actually perform handshaking.
By writing to the Modem Control Register, it will set those lines active.


Bit	|	Name					|	Meaning
------------------------------------------------------------------------------------------------------------------------
0		Data Terminal Ready (DTR)	Controls the Data Terminal Ready Pin
1		Request to Send (RTS)		Controls the Request to Send Pin
2		Out 1						Controls a hardware pin (OUT1) which is unused in PC implementations
3		Out 2						Controls a hardware pin (OUT2) which is used to enable the IRQ in PC implementations
4		Loop						Provides a local loopback feature for diagnostic testing of the UART
5		0							Unused
6		0							Unused
7		0							Unused

Most PC serial ports use OUT2 to control a circuit that disconnects (tristates) the IRQ line.
This makes it possible for multiple serial ports to share a single IRQ line, as long as only one port is enabled at a time.
Loopback mode is a diagnostic feature.
When bit 4 is set to logic 1, the following occur the transmitter Serial Output (SOUT) is set to the Marking (logic 1) state; 
the receiver Serial Input (SIN) is disconnected;
the output of the Transmitter Shift Register is ‘‘looped back’’ into the Receiver Shift Register input;
the four MODEM Control inputs (DSR, CTS, RI, and DCD) are disconnected;
and the four MODEM Control outputs (DTR, RTS, OUT 1, and OUT 2) are internally connected to the four MODEM Control inputs, and the MODEM Control output pins are forced to their inactive state (high).
In the loopback mode, data that is transmitted is immediately received.
This feature allows the processor to verify the transmit-and received- data paths of the UART.
In the loopback mode, the receiver and transmitter interrupts are fully operational.
Their sources are external to the part. The MODEM Control Interrupts are also operational, but the interrupts’ sources are now the lower four bits of the MODEM Control Register instead of the four MODEM Control inputs.
The interrupts are still controlled by the Interrupt Enable Register.
*/

typedef struct {
	uint8_t dtr : 1;
	uint8_t rts : 1;
	uint8_t out1 : 1;
	uint8_t out2 : 1;
	uint8_t loop : 1;
	uint8_t reserved : 3;
} __attribute__((packed)) uart_mcr_t;

// Line Status Register
/*
The line status register is useful to check for errors and enable polling.

Bit	|	Name									|	Meaning
-----------------------------------------------------------------------------------------------------------------------
0		Data ready (DR)								Set if there is data that can be read
1		Overrun error (OE)							Set if there has been data lost
2		Parity error (PE)							Set if there was an error in the transmission as detected by parity
3		Framing error (FE)							Set if a stop bit was missing
4		Break indicator (BI)						Set if there is a break in data input
5		Transmitter holding register empty (THRE)	Set if the transmission buffer is empty (i.e. data can be sent)
6		Transmitter empty (TEMT)					Set if the transmitter is not doing anything
7		Impending Error								Set if there is an error with a word in the input buffer
*/

typedef struct {
	uint8_t data_ready : 1;
	uint8_t overrun_error : 1;
	uint8_t parity_error : 1;
	uint8_t framing_error : 1;
	uint8_t break_indicator : 1;
	uint8_t transmitter_holding_register_empty : 1;
	uint8_t transmitter_empty : 1;
	uint8_t impending_error : 1;
} __attribute__((packed)) uart_lsr_t;

// Modem Status Register
/*
This register provides the current state of the control lines from a peripheral device.
In addition to this current-state information, four bits of the MODEM Status Register provide change information.
These bits are set to a logic 1 whenever a control input from the MODEM changes state.
They are reset to logic 0 whenever the CPU reads the MODEM Status Register


Bit	|	Name									|	Meaning
-------------------------------------------------------------------------------------------------------------------------------
0		Delta Clear to Send (DCTS)					Indicates that CTS input has changed state since the last time it was read
1		Delta Data Set Ready (DDSR)					Indicates that DSR input has changed state since the last time it was read
2		Trailing Edge of Ring Indicator (TERI)		Indicates that RI input to the chip has changed from a low to a high state
3		Delta Data Carrier Detect (DDCD)			Indicates that DCD input has changed state since the last time it ware read
4		Clear to Send (CTS)							Inverted CTS Signal
5		Data Set Ready (DSR)						Inverted DSR Signal
6		Ring Indicator (RI)							Inverted RI Signal
7		Data Carrier Detect (DCD)					Inverted DCD Signal

If Bit 4 of the MCR (LOOP bit) is set, the upper 4 bits will mirror the 4 status output lines set in the Modem Control Register.
*/

typedef struct {
	uint8_t delta_clear_to_send : 1;
	uint8_t delta_data_set_ready : 1;
	uint8_t trailing_edge_of_ring_indicator : 1;
	uint8_t delta_data_carrier_detect : 1;
	uint8_t clear_to_send : 1;
	uint8_t data_set_ready : 1;
	uint8_t ring_indicator : 1;
	uint8_t data_carrier_detect : 1;
} __attribute__((packed)) uart_msr_t;

// main lyr API
int uart_init();
void uart_wbuf(const char *buf, size_t len);
void uart_wstr(const char *str);
void uart_wch(char c);
size_t uart_drain(size_t budget);
void uart_flush(void);
size_t uart_dropped_bytes(void);
int uart_try_read(uint8_t *ch);

#endif // _LYR_DEV_UART_H
