/*
             LUFA Library
     Copyright (C) Dean Camera, 2011.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org
*/

/*
  Copyright 2011  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaim all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

/** \file
 *
 *  Main source file for the CDC class bootloader. This file contains the complete bootloader logic.
 */

#define  INCLUDE_FROM_CATERINA_C
#include "Caterina.h"

/** Contains the current baud rate and other settings of the first virtual serial port. This must be retained as some
 *  operating systems will not open the port unless the settings can be set successfully.
 */
static CDC_LineEncoding_t LineEncoding = { .BaudRateBPS = 0,
                                           .CharFormat  = CDC_LINEENCODING_OneStopBit,
                                           .ParityType  = CDC_PARITY_None,
                                           .DataBits    = 8                            };

/** Current address counter. This stores the current address of the FLASH or EEPROM as set by the host,
 *  and is used when reading or writing to the AVRs memory (either FLASH or EEPROM depending on the issued
 *  command.)
 */
static uint32_t CurrAddress;

/** Flag to indicate if the bootloader should be running, or should exit and allow the application code to run
 *  via a watchdog reset. When cleared the bootloader will exit, starting the watchdog and entering an infinite
 *  loop until the AVR restarts and the application runs.
 */
static bool RunBootloader = true;

/* Pulse generation counters to keep track of the time remaining for each pulse type */
#define TX_RX_LED_PULSE_PERIOD 100
uint16_t TxLEDPulse = 0; // time remaining for Tx LED pulse
uint16_t RxLEDPulse = 0; // time remaining for Rx LED pulse

/* Bootloader timeout timer */
#define TIMEOUT_PERIOD	8000
uint16_t Timeout = 0;

uint16_t bootKey = 0x7777;
volatile uint16_t *const bootKeyPtr = (volatile uint16_t *)0x0800;

void StartSketch(void)
{
	cli();
	
	/* Undo TIMER1 setup and clear the count before running the sketch */
	TIMSK1 = 0;
	TCCR1B = 0;
	TCNT1H = 0;		// 16-bit write to TCNT1 requires high byte be written first
	TCNT1L = 0;
	
	/* Relocate the interrupt vector table to the application section */
	MCUCR = (1 << IVCE);
	MCUCR = 0;

	L_LED_OFF();
	TX_LED_OFF();
	RX_LED_OFF();

	/* jump to beginning of application space */
	__asm__ volatile("jmp 0x0000");
}

/*	Breathing animation on L LED indicates bootloader is running */
uint16_t LLEDPulse;
void LEDPulse(void)
{
	LLEDPulse++;
	uint8_t p = LLEDPulse >> 8;
	if (p > 127)
		p = 254-p;
	p += p;
	if (((uint8_t)LLEDPulse) > p)
		L_LED_OFF();
	else
		L_LED_ON();
}

/** Main program entry point. This routine configures the hardware required by the bootloader, then continuously
 *  runs the bootloader processing routine until it times out or is instructed to exit.
 */
int main(void)
{
	/* Save the value of the boot key memory before it is overwritten */
	uint16_t bootKeyPtrVal = *bootKeyPtr;
	*bootKeyPtr = 0;

	/* Check the reason for the reset so we can act accordingly */
	uint8_t  mcusr_state = MCUSR;		// store the initial state of the Status register
	MCUSR = 0;							// clear all reset flags	

	/* Watchdog may be configured with a 15 ms period so must disable it before going any further */
	wdt_disable();
	
	if (mcusr_state & (1<<EXTRF)) {
		// External reset -  we should continue to self-programming mode.
	} else if ((mcusr_state & (1<<PORF)) && (pgm_read_word(0) != 0xFFFF)) {		
		// After a power-on reset allow only a small amount to bootloader time.
		Timeout = TIMEOUT_PERIOD / 4;
	} else if ((mcusr_state & (1<<WDRF)) && (bootKeyPtrVal != bootKey) && (pgm_read_word(0) != 0xFFFF)) {	
		// If it looks like an "accidental" watchdog reset then start the sketch.
		StartSketch();
	}
	
	/* Setup hardware required for the bootloader */
	SetupHardware();

	/* Enable global interrupts so that the USB stack can function */
	sei();
	
	Timeout = 0;
	
	while (RunBootloader)
	{
		CDC_Task();
		USB_USBTask();
		/* Time out and start the sketch if one is present */
		if (Timeout > TIMEOUT_PERIOD)
			RunBootloader = false;

		LEDPulse();
	}

	/* Disconnect from the host - USB interface will be reset later along with the AVR */
	USB_Detach();

	/* Jump to beginning of application space to run the sketch - do not reset */	
	StartSketch();
}

/** Configures all hardware required for the bootloader. */
void SetupHardware(void)
{
	/* Disable watchdog if enabled by bootloader/fuses */
	MCUSR &= ~(1 << WDRF);
	wdt_disable();

	/* Disable clock division */
	clock_prescale_set(clock_div_1);

	/* Relocate the interrupt vector table to the bootloader section */
	MCUCR = (1 << IVCE);
	MCUCR = (1 << IVSEL);
	
	LED_SETUP();
	CPU_PRESCALE(0); 
	L_LED_OFF();
	TX_LED_OFF();
	RX_LED_OFF();
	
	/* Initialize TIMER1 to handle bootloader timeout and LED tasks.  
	 * With 16 MHz clock and 1/64 prescaler, timer 1 is clocked at 250 kHz
	 * Our chosen compare match generates an interrupt every 1 ms.
	 * This interrupt is disabled selectively when doing memory reading, erasing,
	 * or writing since SPM has tight timing requirements.
	 */ 
	OCR1AH = 0;
	OCR1AL = 250;
	TIMSK1 = (1 << OCIE1A);					// enable timer 1 output compare A match interrupt
	TCCR1B = ((1 << CS11) | (1 << CS10));	// 1/64 prescaler on timer 1 input

	/* Initialize USB Subsystem */
	USB_Init();
}

//uint16_t ctr = 0;
ISR(TIMER1_COMPA_vect, ISR_BLOCK)
{
	/* Reset counter */
	TCNT1H = 0;
	TCNT1L = 0;

	/* Check whether the TX or RX LED one-shot period has elapsed.  if so, turn off the LED */
	if (TxLEDPulse && !(--TxLEDPulse))
		TX_LED_OFF();
	if (RxLEDPulse && !(--RxLEDPulse))
		RX_LED_OFF();
	
	if (pgm_read_word(0) != 0xFFFF)
		Timeout++;
}

/** Event handler for the USB_ConfigurationChanged event. This configures the device's endpoints ready
 *  to relay data to and from the attached USB host.
 */
void EVENT_USB_Device_ConfigurationChanged(void)
{
	/* Setup CDC Notification, Rx and Tx Endpoints */
	Endpoint_ConfigureEndpoint(CDC_NOTIFICATION_EPNUM, EP_TYPE_INTERRUPT,
	                           ENDPOINT_DIR_IN, CDC_NOTIFICATION_EPSIZE,
	                           ENDPOINT_BANK_SINGLE);

	Endpoint_ConfigureEndpoint(CDC_TX_EPNUM, EP_TYPE_BULK,
	                           ENDPOINT_DIR_IN, CDC_TXRX_EPSIZE,
	                           ENDPOINT_BANK_SINGLE);

	Endpoint_ConfigureEndpoint(CDC_RX_EPNUM, EP_TYPE_BULK,
	                           ENDPOINT_DIR_OUT, CDC_TXRX_EPSIZE,
	                           ENDPOINT_BANK_SINGLE);
}

/** Event handler for the USB_ControlRequest event. This is used to catch and process control requests sent to
 *  the device from the USB host before passing along unhandled control requests to the library for processing
 *  internally.
 */
void EVENT_USB_Device_ControlRequest(void)
{
	/* Ignore any requests that aren't directed to the CDC interface */
	if ((USB_ControlRequest.bmRequestType & (CONTROL_REQTYPE_TYPE | CONTROL_REQTYPE_RECIPIENT)) !=
	    (REQTYPE_CLASS | REQREC_INTERFACE))
	{
		return;
	}

	/* Process CDC specific control requests */
	switch (USB_ControlRequest.bRequest)
	{
		case CDC_REQ_GetLineEncoding:
			if (USB_ControlRequest.bmRequestType == (REQDIR_DEVICETOHOST | REQTYPE_CLASS | REQREC_INTERFACE))
			{
				Endpoint_ClearSETUP();

				/* Write the line coding data to the control endpoint */
				Endpoint_Write_Control_Stream_LE(&LineEncoding, sizeof(CDC_LineEncoding_t));
				Endpoint_ClearOUT();
			}

			break;
		case CDC_REQ_SetLineEncoding:
			if (USB_ControlRequest.bmRequestType == (REQDIR_HOSTTODEVICE | REQTYPE_CLASS | REQREC_INTERFACE))
			{
				Endpoint_ClearSETUP();

				/* Read the line coding data in from the host into the global struct */
				Endpoint_Read_Control_Stream_LE(&LineEncoding, sizeof(CDC_LineEncoding_t));
				Endpoint_ClearIN();
			}

			break;
	}
}

#if !defined(NO_BLOCK_SUPPORT)
/** Reads or writes a block of EEPROM or FLASH memory to or from the appropriate CDC data endpoint, depending
 *  on the AVR910 protocol command issued.
 *
 *  \param[in] Command  Single character AVR910 protocol command indicating what memory operation to perform
 */
static void ReadWriteMemoryBlock(const uint8_t Command)
{
	uint16_t BlockSize;
	char     MemoryType;

	bool     HighByte = false;
	uint8_t  LowByte  = 0;

	BlockSize  = (FetchNextCommandByte() << 8);
	BlockSize |=  FetchNextCommandByte();

	MemoryType =  FetchNextCommandByte();

	if ((MemoryType != 'E') && (MemoryType != 'F'))
	{
		/* Send error byte back to the host */
		WriteNextResponseByte('?');

		return;
	}

	/* Disable timer 1 interrupt - can't afford to process nonessential interrupts
	 * while doing SPM tasks */
	TIMSK1 = 0;

	/* Check if command is to read memory */
	if (Command == 'g')
	{		
		/* Re-enable RWW section */
		boot_rww_enable();

		while (BlockSize--)
		{
			if (MemoryType == 'F')
			{
				/* Read the next FLASH byte from the current FLASH page */
				#if (FLASHEND > 0xFFFF)
				WriteNextResponseByte(pgm_read_byte_far(CurrAddress | HighByte));
				#else
				WriteNextResponseByte(pgm_read_byte(CurrAddress | HighByte));
				#endif

				/* If both bytes in current word have been read, increment the address counter */
				if (HighByte)
				  CurrAddress += 2;

				HighByte = !HighByte;
			}
			else
			{
				/* Read the next EEPROM byte into the endpoint */
				WriteNextResponseByte(eeprom_read_byte((uint8_t*)(intptr_t)(CurrAddress >> 1)));

				/* Increment the address counter after use */
				CurrAddress += 2;
			}
		}
	}
	else
	{
		uint32_t PageStartAddress = CurrAddress;

		if (MemoryType == 'F')
		{
			boot_page_erase(PageStartAddress);
			boot_spm_busy_wait();
		}

		while (BlockSize--)
		{
			if (MemoryType == 'F')
			{
				/* If both bytes in current word have been written, increment the address counter */
				if (HighByte)
				{
					/* Write the next FLASH word to the current FLASH page */
					boot_page_fill(CurrAddress, ((FetchNextCommandByte() << 8) | LowByte));

					/* Increment the address counter after use */
					CurrAddress += 2;
				}
				else
				{
					LowByte = FetchNextCommandByte();
				}
				
				HighByte = !HighByte;
			}
			else
			{
				/* Write the next EEPROM byte from the endpoint */
				eeprom_write_byte((uint8_t*)((intptr_t)(CurrAddress >> 1)), FetchNextCommandByte());

				/* Increment the address counter after use */
				CurrAddress += 2;
			}
		}

		/* If in FLASH programming mode, commit the page after writing */
		if (MemoryType == 'F')
		{
			/* Commit the flash page to memory */
			boot_page_write(PageStartAddress);

			/* Wait until write operation has completed */
			boot_spm_busy_wait();
		}

		/* Send response byte back to the host */
		WriteNextResponseByte('\r');
	}

	/* Re-enable timer 1 interrupt disabled earlier in this routine */	
	TIMSK1 = (1 << OCIE1A);
}
#endif

/** Retrieves the next byte from the host in the CDC data OUT endpoint, and clears the endpoint bank if needed
 *  to allow reception of the next data packet from the host.
 *
 *  \return Next received byte from the host in the CDC data OUT endpoint
 */
static uint8_t FetchNextCommandByte(void)
{
	/* Select the OUT endpoint so that the next data byte can be read */
	Endpoint_SelectEndpoint(CDC_RX_EPNUM);

	/* If OUT endpoint empty, clear it and wait for the next packet from the host */
	while (!(Endpoint_IsReadWriteAllowed()))
	{
		Endpoint_ClearOUT();

		while (!(Endpoint_IsOUTReceived()))
		{
			if (USB_DeviceState == DEVICE_STATE_Unattached)
			  return 0;
		}
	}

	/* Fetch the next byte from the OUT endpoint */
	return Endpoint_Read_8();
}

/** Writes the next response byte to the CDC data IN endpoint, and sends the endpoint back if needed to free up the
 *  bank when full ready for the next byte in the packet to the host.
 *
 *  \param[in] Response  Next response byte to send to the host
 */
static void WriteNextResponseByte(const uint8_t Response)
{
	/* Select the IN endpoint so that the next data byte can be written */
	Endpoint_SelectEndpoint(CDC_TX_EPNUM);

	/* If IN endpoint full, clear it and wait until ready for the next packet to the host */
	if (!(Endpoint_IsReadWriteAllowed()))
	{
		Endpoint_ClearIN();

		while (!(Endpoint_IsINReady()))
		{
			if (USB_DeviceState == DEVICE_STATE_Unattached)
			  return;
		}
	}

	/* Write the next byte to the IN endpoint */
	Endpoint_Write_8(Response);
	
	TX_LED_ON();
	TxLEDPulse = TX_RX_LED_PULSE_PERIOD;
}

#define STK_OK              0x10
#define STK_INSYNC          0x14  // ' '
#define CRC_EOP             0x20  // 'SPACE'
#define STK_GET_SYNC        0x30  // '0'

#define STK_GET_PARAMETER   0x41  // 'A'
#define STK_SET_DEVICE      0x42  // 'B'
#define STK_SET_DEVICE_EXT  0x45  // 'E'
#define STK_LOAD_ADDRESS    0x55  // 'U'
#define STK_UNIVERSAL       0x56  // 'V'
#define STK_PROG_PAGE       0x64  // 'd'
#define STK_READ_PAGE       0x74  // 't'
#define STK_READ_SIGN       0x75  // 'u'

/** Task to read in AVR910 commands from the CDC data OUT endpoint, process them, perform the required actions
 *  and send the appropriate response back to the host.
 */
void CDC_Task(void)
{
	/* Select the OUT endpoint */
	Endpoint_SelectEndpoint(CDC_RX_EPNUM);

	/* Check if endpoint has a command in it sent from the host */
	if (!(Endpoint_IsOUTReceived()))
	  return;
	  
	RX_LED_ON();
	RxLEDPulse = TX_RX_LED_PULSE_PERIOD;

	/* Read in the bootloader command (first byte sent from host) */
	uint8_t Command = FetchNextCommandByte();

	if (Command == 'E')
	{
		/* We nearly run out the bootloader timeout clock, 
		* leaving just a few hundred milliseconds so the 
		* bootloder has time to respond and service any 
		* subsequent requests */
		Timeout = TIMEOUT_PERIOD - 500;
	
		/* Re-enable RWW section - must be done here in case 
		 * user has disabled verification on upload.  */
		boot_rww_enable_safe();		

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 'T')
	{
		FetchNextCommandByte();

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if ((Command == 'L') || (Command == 'P'))
	{
		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 't')
	{
		// Return ATMEGA128 part code - this is only to allow AVRProg to use the bootloader 
		WriteNextResponseByte(0x44);
		WriteNextResponseByte(0x00);
	}
	else if (Command == 'a')
	{
		// Indicate auto-address increment is supported 
		WriteNextResponseByte('Y');
	}
	else if (Command == 'A')
	{
		// Set the current address to that given by the host 
		CurrAddress   = (FetchNextCommandByte() << 9);
		CurrAddress  |= (FetchNextCommandByte() << 1);

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 'p')
	{
		// Indicate serial programmer back to the host 
		WriteNextResponseByte('S');
	}
	else if (Command == 'S')
	{
		// Write the 7-byte software identifier to the endpoint 
		for (uint8_t CurrByte = 0; CurrByte < 7; CurrByte++)
		  WriteNextResponseByte(SOFTWARE_IDENTIFIER[CurrByte]);
	}
	else if (Command == 'V')
	{
		WriteNextResponseByte('0' + BOOTLOADER_VERSION_MAJOR);
		WriteNextResponseByte('0' + BOOTLOADER_VERSION_MINOR);
	}
	else if (Command == 's')
	{
		WriteNextResponseByte(AVR_SIGNATURE_3);
		WriteNextResponseByte(AVR_SIGNATURE_2);
		WriteNextResponseByte(AVR_SIGNATURE_1);
	}
	else if (Command == 'e')
	{
		// Clear the application section of flash 
		for (uint32_t CurrFlashAddress = 0; CurrFlashAddress < BOOT_START_ADDR; CurrFlashAddress += SPM_PAGESIZE)
		{
			boot_page_erase(CurrFlashAddress);
			boot_spm_busy_wait();
			boot_page_write(CurrFlashAddress);
			boot_spm_busy_wait();
		}

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	#if !defined(NO_LOCK_BYTE_WRITE_SUPPORT)
	else if (Command == 'l')
	{
		// Set the lock bits to those given by the host 
		boot_lock_bits_set(FetchNextCommandByte());

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	#endif
	else if (Command == 'r')
	{
		WriteNextResponseByte(boot_lock_fuse_bits_get(GET_LOCK_BITS));
	}
	else if (Command == 'F')
	{
		WriteNextResponseByte(boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS));
	}
	else if (Command == 'N')
	{
		WriteNextResponseByte(boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS));
	}
	else if (Command == 'Q')
	{
		WriteNextResponseByte(boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS));
	}
	#if !defined(NO_BLOCK_SUPPORT)
	else if (Command == 'b')
	{
		WriteNextResponseByte('Y');

		// Send block size to the host 
		WriteNextResponseByte(SPM_PAGESIZE >> 8);
		WriteNextResponseByte(SPM_PAGESIZE & 0xFF);
	}
	else if ((Command == 'B') || (Command == 'g'))
	{
		// Keep resetting the timeout counter if we're receiving self-programming instructions
		Timeout = 0;
		// Delegate the block write/read to a separate function for clarity 
		ReadWriteMemoryBlock(Command);
	}
	#endif
	#if !defined(NO_FLASH_BYTE_SUPPORT)
	else if (Command == 'C')
	{
		// Write the high byte to the current flash page
		boot_page_fill(CurrAddress, FetchNextCommandByte());

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 'c')
	{
		// Write the low byte to the current flash page 
		boot_page_fill(CurrAddress | 0x01, FetchNextCommandByte());

		// Increment the address 
		CurrAddress += 2;

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 'm')
	{
		// Commit the flash page to memory
		boot_page_write(CurrAddress);

		// Wait until write operation has completed 
		boot_spm_busy_wait();

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 'R')
	{
		#if (FLASHEND > 0xFFFF)
		uint16_t ProgramWord = pgm_read_word_far(CurrAddress);
		#else
		uint16_t ProgramWord = pgm_read_word(CurrAddress);
		#endif

		WriteNextResponseByte(ProgramWord >> 8);
		WriteNextResponseByte(ProgramWord & 0xFF);
	}
	#endif
	#if !defined(NO_EEPROM_BYTE_SUPPORT)
	else if (Command == 'D')
	{
		// Read the byte from the endpoint and write it to the EEPROM 
		eeprom_write_byte((uint8_t*)((intptr_t)(CurrAddress >> 1)), FetchNextCommandByte());

		// Increment the address after use
		CurrAddress += 2;

		// Send confirmation byte back to the host 
		WriteNextResponseByte('\r');
	}
	else if (Command == 'd')
	{
		// Read the EEPROM byte and write it to the endpoint 
		WriteNextResponseByte(eeprom_read_byte((uint8_t*)((intptr_t)(CurrAddress >> 1))));

		// Increment the address after use 
		CurrAddress += 2;
	}
	#endif
	else if (Command != 27)
	{
		// Unknown (non-sync) command, return fail code 
		WriteNextResponseByte('?');
	}
	

	/* Select the IN endpoint */
	Endpoint_SelectEndpoint(CDC_TX_EPNUM);

	/* Remember if the endpoint is completely full before clearing it */
	bool IsEndpointFull = !(Endpoint_IsReadWriteAllowed());

	/* Send the endpoint data to the host */
	Endpoint_ClearIN();

	/* If a full endpoint's worth of data was sent, we need to send an empty packet afterwards to signal end of transfer */
	if (IsEndpointFull)
	{
		while (!(Endpoint_IsINReady()))
		{
			if (USB_DeviceState == DEVICE_STATE_Unattached)
			  return;
		}

		Endpoint_ClearIN();
	}

	/* Wait until the data has been sent to the host */
	while (!(Endpoint_IsINReady()))
	{
		if (USB_DeviceState == DEVICE_STATE_Unattached)
		  return;
	}

	/* Select the OUT endpoint */
	Endpoint_SelectEndpoint(CDC_RX_EPNUM);

	/* Acknowledge the command from the host */
	Endpoint_ClearOUT();
}

