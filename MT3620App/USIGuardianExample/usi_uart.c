/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#include "usi_uart.h"
#define UART_READ_DATA_LENGTH 256

// File descriptors - initialized to invalid value
static int uartFd = -1;
static char uartReadData[UART_READ_DATA_LENGTH] = "\0";
static char *sendToCloudPropertyName = "sendToCloud";

   /// <summary>
   ///     Signal handler for termination requests. This handler must be async-signal-safe.
   /// </summary>
static void TerminationHandler(int signalNumber)
{
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
}

/// <summary>
///     Helper function to send a fixed message via the given UART.
/// </summary>
/// <param name="uartFd">The open file descriptor of the UART to write to</param>
/// <param name="dataToSend">The data to send over the UART</param>
static void SendUartMessage(int uartFd, const char *dataToSend)
{
	size_t totalBytesSent = 0;
	size_t totalBytesToSend = strlen(dataToSend);
	int sendIterations = 0;
	while (totalBytesSent < totalBytesToSend) {
		sendIterations++;

		// Send as much of the remaining data as possible
		size_t bytesLeftToSend = totalBytesToSend - totalBytesSent;
		const char *remainingMessageToSend = dataToSend + totalBytesSent;
		ssize_t bytesSent = write(uartFd, remainingMessageToSend, bytesLeftToSend);
		if (bytesSent < 0) {
			Log_Debug("ERROR: Could not write to UART: %s (%d).\n", strerror(errno), errno);
			terminationRequired = true;
			return;
		}

		totalBytesSent += (size_t)bytesSent;
	}

	Log_Debug("Sent %zu bytes over UART in %d calls.\n", totalBytesSent, sendIterations);
}

/// <summary>
///     Handle UART event: if there is incoming data, print it.
/// </summary>
static void UartEventHandler(EventData *eventData)
{
	const size_t receiveBufferSize = 256;
	uint8_t receiveBuffer[receiveBufferSize + 1]; // allow extra byte for string termination
	ssize_t bytesRead;

	// Read incoming UART data. It is expected behavior that messages may be received in multiple
	// partial chunks.
	bytesRead = read(uartFd, receiveBuffer, receiveBufferSize);
	if (bytesRead < 0) {
		Log_Debug("ERROR: Could not read UART: %s (%d).\n", strerror(errno), errno);
		terminationRequired = true;
		return;
	}

	if (bytesRead > 0) {
		// Null terminate the buffer to make it a valid string, and print it
		receiveBuffer[bytesRead] = 0;
		Log_Debug("UART received %d bytes: '%s'.\n", bytesRead, (char *)receiveBuffer);

		//When buffer more than 256 bytes will send message to Azure IoT
		if (bytesRead + (int)strlen(uartReadData) >= UART_READ_DATA_LENGTH) {
			USIAzureIoT_SendStringToCloud(sendToCloudPropertyName, uartReadData);
			memset(uartReadData, '\0', sizeof(uartReadData));
		}

		strcat(uartReadData, receiveBuffer);
		//When receive data include 0x0d('\r') will send message to Azure IoT
		if (receiveBuffer[bytesRead - 1] == 0x0d) {
			USIAzureIoT_SendStringToCloud(sendToCloudPropertyName, uartReadData);
			memset(uartReadData, '\0', sizeof(uartReadData));
		}
	}
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData uartEventData = { .eventHandler = &UartEventHandler };

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitPeripheralsAndHandlers(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = TerminationHandler;
	sigaction(SIGTERM, &action, NULL);

	// Create a UART_Config object, open the UART and set up UART event handler
	UART_Config uartConfig;
	UART_InitConfig(&uartConfig);
	uartConfig.baudRate = 115200;
	uartConfig.flowControl = UART_FlowControl_None;
	uartFd = UART_Open(USI_MT3620_BT_GB_ISU2_UART, &uartConfig);
	if (uartFd < 0) {
		Log_Debug("ERROR: Could not open UART: %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	if (RegisterEventHandlerToEpoll(epollFd, uartFd, &uartEventData, EPOLLIN) != 0) {
		return -1;
	}

	return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors.\n");
	CloseFdAndPrintError(uartFd, "Uart");
}

int USIUart_Init(int usiuart_epollFd, sig_atomic_t usiuart_terminationRequired) {
	Log_Debug("INFO: USI UART starting.\n");

	terminationRequired = usiuart_terminationRequired;
	epollFd = usiuart_epollFd;

	if (InitPeripheralsAndHandlers() != 0) {
		return -1;
	}
	return 0;
}

void USIUart_Deinit(void) {
	ClosePeripheralsAndHandlers();
}

void USIUart_SendUartMsg(const char *dataToSend) {
	SendUartMessage(uartFd,dataToSend);
}