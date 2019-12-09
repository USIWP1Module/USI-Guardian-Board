/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#include "common.h"
#include "wifisetupbybt.h"
#include "usi_uart.h"
#include "usi_private_ethernet.h"
#include "usi_azureiot.h"
#include "usi_rs232_485.h"
#include "nordic/dfu_uart_protocol.h"

static int nrfUartFd = -1;
static int nrfResetGpioFd = -1;
static int nrfDfuModeGpioFd = -1;
   // To write an image to the Nordic board, add the data and binary files as
   // resources to the solution and modify this object. The first image should
   // be the softdevice; the second image is the application.
static DfuImageData images[] = { {.datPathname = "s132_nrf52_6.1.0_softdevice.dat",
								 .binPathname = "s132_nrf52_6.1.0_softdevice.bin",
								 .firmwareType = DfuFirmware_Softdevice,
								 .version = 6001000},
								{.datPathname = "nrf52832_WiFiSetupByBT.dat",
								 .binPathname = "nrf52832_WiFiSetupByBT.bin",
								 .firmwareType = DfuFirmware_Application,
								 .version = 1} };

static const size_t imageCount = sizeof(images) / sizeof(images[0]);

// Whether currently writing images to attached board.
static bool inDfuMode = false;

// Termination state
volatile sig_atomic_t terminationRequired = false;
int epollFd = -1;

void DfuTerminationHandler(DfuResultStatus status);
static int updateBleFw(void);
static int InitDFUPeripheralsAndHandlers(void);
static void CloseDFUPeripheralsAndHandlers(void);

void DfuTerminationHandler(DfuResultStatus status)
{
	Log_Debug("\nFinished updating images with status: %s, setting DFU mode to false.\n",
		status == DfuResult_Success ? "SUCCESS" : "FAILED");
	inDfuMode = false;

	//After update BLE FW will close DFU GPIO and UART
	CloseDFUPeripheralsAndHandlers();

	//If BLE FW update Fail or in DFU mode will not init WiFiSetupByBT function
	if ((status == DfuResult_Fail) || (inDfuMode))
		return;

#if (defined(BUILD_USI_WIFISETUPBYBT) )
	if (WiFiSetupByBT_Init(epollFd, terminationRequired) != 0) {
		terminationRequired = true;
		Log_Debug("Init WiFiSetupByBT Fail\n");
	}
#endif
}

static int updateBleFw(void) {

	// Take nRF52 out of reset, allowing its application to start
	GPIO_SetValue(nrfResetGpioFd, GPIO_Value_High);

	Log_Debug("\nStarting firmware update...\n");
	inDfuMode = true;
	ProgramImages(images, imageCount, &DfuTerminationHandler);
}

static int InitDFUPeripheralsAndHandlers(void)
{
	nrfResetGpioFd =
		GPIO_OpenAsOutput(USI_NRF52_RESET, GPIO_OutputMode_OpenDrain, GPIO_Value_High);
	if (nrfResetGpioFd == -1) {
		Log_Debug("ERROR: Could not open SAMPLE_NRF52_RESET: %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	GPIO_SetValue(nrfResetGpioFd, GPIO_Value_Low);

	// Create a UART_Config object, open the UART and set up UART event handler
	UART_Config uartConfig;
	UART_InitConfig(&uartConfig);
	uartConfig.baudRate = 115200;
	uartConfig.flowControl = UART_FlowControl_RTSCTS;
	nrfUartFd = UART_Open(USI_NRF52_UART, &uartConfig);
	if (nrfUartFd == -1) {
		Log_Debug("ERROR: Could not open UART: %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	// uartFd will be added to the epoll when needed (check epoll protocol)

	nrfDfuModeGpioFd =
		GPIO_OpenAsOutput(USI_NRF52_DFU, GPIO_OutputMode_OpenDrain, GPIO_Value_High);
	if (nrfDfuModeGpioFd == -1) {
		Log_Debug("ERROR: Could not open SAMPLE_NRF52_DFU: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	InitUartProtocol(nrfUartFd, nrfResetGpioFd, nrfDfuModeGpioFd, epollFd);

	return 0;
}

static void CloseDFUPeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors\n");
	CloseFdAndPrintError(nrfResetGpioFd, "NrfResetGpio");
	CloseFdAndPrintError(nrfDfuModeGpioFd, "NrfDfuModeGpio");
	CloseFdAndPrintError(nrfUartFd, "NrfUart");
}

/// <summary>
///     Main entry point for this application.
/// </summary>
int main(int argc, char *argv[])
{
	epollFd = CreateEpollFd();
	if (epollFd < 0) {
		terminationRequired = true;
	}

	// Update BLE FW
	InitDFUPeripheralsAndHandlers();
	updateBleFw();

#if (defined(BUILD_USI_UART) )
	if (USIUart_Init(epollFd, terminationRequired) != 0) {
		terminationRequired = true;
		Log_Debug("Init USI UART Fail\n");
	}
#endif

#if (defined(BUILD_USI_RS232_485) )
	if (USIRs_Init(epollFd, terminationRequired) != 0) {
		terminationRequired = true;
		Log_Debug("Init USI RS232&485 Fail\n");
	}
#endif

#if (defined(BUILD_USI_PRIVATE_ETHERNET) )
	if (USIPrivateEthernet_Init(epollFd, terminationRequired) != 0) {
		terminationRequired = true;
		Log_Debug("Init USI Private Ethernet Fail\n");
	}
#endif

	if (argc == 2) {
		Log_Debug("Setting Azure Scope ID %s\n", argv[1]);
		if (USIAzureIoT_Init(epollFd, terminationRequired, argv[1]) != 0) {
			terminationRequired = true;
			Log_Debug("Init USI Azure IoT Fail\n");
		}
	}
	else {
		Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
		terminationRequired = true;
	}


    // Use epoll to wait for events and trigger handlers, until an error or SIGTERM happens
    while (!terminationRequired) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequired = true;
        }
    }

#if (defined(BUILD_USI_WIFISETUPBYBT) )
	WiFiSetupByBT_Deinit();
#endif
#if (defined(BUILD_USI_UART) )
	USIUart_Deinit();
#endif
#if (defined(BUILD_USI_RS232_485) )
	USIRs_Deinit();
#endif
#if (defined(BUILD_USI_PRIVATE_ETHERNET) )
	USIPrivateEthernet_Deinit();
#endif
	USIAzureIoT_Deinit();
    Log_Debug("INFO: Application exiting\n");
    return 0;
}