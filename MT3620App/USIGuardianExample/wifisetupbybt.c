#include "wifisetupbybt.h"

// This application forms part of the Wi-Fi setup and device control via BLE reference solution for
// Azure Sphere.
//
// It implements communication between an Azure Sphere MCU and the sibling application
// running on a Nordic nRF52 Bluetooth LE board, allowing Wi-Fi configuration and LED control on the
// Azure Sphere via Bluetooth LE.
//
// Pressing USI_BUTTON_1 briefly will start allowing new BLE bonds for 1 minute.
// Holding USI_BUTTON_1 will delete all BLE bonds.
// Pressing USI_BUTTON_2 briefly will toggle USI_WIFI_LED_RED.
// Holding USI_BUTTON_2 will forget all stored Wi-Fi networks on Azure Sphere.
// USI_BT_LED will be illuminated to a color which indicates the BLE status:
//      Yellow  - Uninitialized;
//      Blue    - Advertising to bonded devices only;
//      Red     - Advertising to all devices;
//      Green   - Connected to a central device;
//      Magenta - Error
//
// It uses the API for the following Azure Sphere application libraries:
// - UART (serial port)
// - GPIO (digital inputs and outputs)
// - log (messages shown in Visual Studio's Device Output window during debugging)
// - wificonfig (configure Wi-Fi settings)

// File descriptors - initialized to invalid value
static int buttonTimerFd = -1;
static int bleAdvertiseToBondedDevicesLedGpioFd = -1;
static int bleAdvertiseToAllDevicesLedGpioFd = -1;
static int bleConnectedLedGpioFd = -1;
static int deviceControlLedGpioFd = -1;
static int uartFd = -1;
static int bleDeviceResetPinGpioFd = -1;
static struct timespec bleAdvertiseToAllTimeoutPeriod = { 60u, 0 };
static GPIO_Value_Type deviceControlLedState = GPIO_Value_High;

/// <summary>
///     Button events.
/// </summary>
typedef enum {
	/// <summary>The event when failing to get button state.</summary>
	ButtonEvent_Error = -1,
	/// <summary>No button event has occurred.</summary>
	ButtonEvent_None = 0,
	/// <summary>The event when button is pressed.</summary>
	ButtonEvent_Pressed,
	/// <summary>The event when button is released.</summary>
	ButtonEvent_Released,
	/// <summary>The event when button is being held.</summary>
	ButtonEvent_Held,
	/// <summary>The event when button is released after being held.</summary>
	ButtonEvent_ReleasedAfterHeld
} ButtonEvent;

/// <summary>
///     Data structure for the button state.
/// </summary>
typedef struct {
	/// <summary>File descriptor for the button.</summary>
	int fd;
	/// <summary>Whether the button is currently pressed.</summary>
	bool isPressed;
	/// <summary>Whether the button is currently held.</summary>
	bool isHeld;
	/// <summary>The time stamp when the button-press happened.</summary>
	struct timespec pressedTime;
} ButtonState;

// Button-related variables
static const time_t buttonHeldThresholdTimeInSeconds = 3L;
static ButtonState button1State = { .fd = -1,.isPressed = false,.isHeld = false };
static ButtonState button2State = { .fd = -1,.isPressed = false,.isHeld = false };

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
}

ButtonEvent GetButtonEvent(ButtonState *state)
{
	ButtonEvent event = ButtonEvent_None;
	GPIO_Value_Type newInputValue;
	int result = GPIO_GetValue(state->fd, &newInputValue);
	if (result != 0) {
		Log_Debug("ERROR: Could not read button GPIO: %s (%d).\n", strerror(errno), errno);
		return ButtonEvent_Error;
	}
	if (state->isPressed) {
		if (newInputValue == GPIO_Value_High) {
			// Button has just been released, so set event based on whether button has been held and
			// then reset flags.
			event = state->isHeld ? ButtonEvent_ReleasedAfterHeld : ButtonEvent_Released;
			state->isHeld = false;
			state->isPressed = false;
		}
		if (newInputValue == GPIO_Value_Low && !state->isHeld) {
			// Button has been pressed and hasn't been released yet. As it hasn't been classified as
			// held, compare the elapsed time to determine whether the button has been held long
			// enough to be regarded as 'Held'.
			struct timespec currentTime;
			clock_gettime(CLOCK_REALTIME, &currentTime);
			long elapsedSeconds = currentTime.tv_sec - state->pressedTime.tv_sec;
			state->isHeld = (elapsedSeconds > buttonHeldThresholdTimeInSeconds ||
				(elapsedSeconds == buttonHeldThresholdTimeInSeconds &&
					currentTime.tv_nsec >= state->pressedTime.tv_nsec));
			if (state->isHeld) {
				event = ButtonEvent_Held;
			}
		}
	}
	else if (newInputValue == GPIO_Value_Low) {
		// Button has just been pressed, set isPressed flag and mark current time.
		state->isPressed = true;
		clock_gettime(CLOCK_REALTIME, &state->pressedTime);
		event = ButtonEvent_Pressed;
	}
	return event;
}

static void UpdateBleLedStatus(BleControlMessageProtocolState state)
{
	GPIO_SetValue(bleAdvertiseToBondedDevicesLedGpioFd,
		(state == BleControlMessageProtocolState_AdvertiseToBondedDevices)
		? GPIO_Value_Low
		: GPIO_Value_High);
	GPIO_SetValue(bleAdvertiseToAllDevicesLedGpioFd,
		(state == BleControlMessageProtocolState_AdvertisingToAllDevices)
		? GPIO_Value_Low
		: GPIO_Value_High);
	GPIO_SetValue(bleConnectedLedGpioFd, (state == BleControlMessageProtocolState_DeviceConnected)
		? GPIO_Value_Low
		: GPIO_Value_High);
	if (state == BleControlMessageProtocolState_Uninitialized) {
		// Illuminate RGB LED to Yellow (Green + Red) to indicate BLE is in the uninitialized state.
		GPIO_SetValue(bleAdvertiseToAllDevicesLedGpioFd, GPIO_Value_Low);
		GPIO_SetValue(bleConnectedLedGpioFd, GPIO_Value_Low);
	}
	else if (state == BleControlMessageProtocolState_Error) {
		// Illuminate RGB LED to Magenta (Blue + Red) to indicate BLE is in the error state.
		GPIO_SetValue(bleAdvertiseToBondedDevicesLedGpioFd, GPIO_Value_Low);
		GPIO_SetValue(bleAdvertiseToAllDevicesLedGpioFd, GPIO_Value_Low);
	}
}

/// <summary>
///     Handle notification of state change generated by the attached BLE device.
/// </summary>
/// <param name="state">The new state of attached BLE device.</param>
static void BleStateChangeHandler(BleControlMessageProtocolState state)
{
	UpdateBleLedStatus(state);
	switch (state) {
	case BleControlMessageProtocolState_Error:
		Log_Debug("INFO: BLE device is in an error state, resetting it...\n");
		GPIO_SetValue(bleDeviceResetPinGpioFd, GPIO_Value_Low);
		GPIO_SetValue(bleDeviceResetPinGpioFd, GPIO_Value_High);
		break;
	case BleControlMessageProtocolState_AdvertiseToBondedDevices:
		Log_Debug("INFO: BLE device is advertising to bonded devices only.\n");
		break;
	case BleControlMessageProtocolState_AdvertisingToAllDevices:
		Log_Debug("INFO: BLE device is advertising to all devices.\n");
		break;
	case BleControlMessageProtocolState_DeviceConnected:
		Log_Debug("INFO: BLE device is now connected to a central device.\n");
		break;
	case BleControlMessageProtocolState_Uninitialized:
		Log_Debug("INFO: BLE device is now being initialized.\n");
		break;
	default:
		Log_Debug("ERROR: Unsupported BLE state: %d.\n", state);
		break;
	}
}

/// <summary>
///     Set the Device Control LED's status.
/// </summary>
/// <param name="state">The LED status to be set.</param>
static void SetDeviceControlLedStatusHandler(bool isOn)
{
	deviceControlLedState = (isOn ? GPIO_Value_Low : GPIO_Value_High);
	GPIO_SetValue(deviceControlLedGpioFd, deviceControlLedState);
}

/// <summary>
///     Get status for the Device Control LED.
/// </summary>
/// <returns>The status of Device Control LED.</returns>
static bool GetDeviceControlLedStatusHandler(void)
{
	return (deviceControlLedState == GPIO_Value_Low);
}

/// <summary>
///     Handle button timer event and take defined actions as printed when the application started.
/// </summary>
/// <param name="eventData">Context data for handled event.</param>
static void ButtonTimerEventHandler(EventData *eventData)
{
	if (ConsumeTimerFdEvent(buttonTimerFd) != 0) {
		terminationRequired = true;
		return;
	}

	// Take actions based on button events.
	ButtonEvent button1Event = GetButtonEvent(&button1State);
	if (button1Event == ButtonEvent_Error) {
		terminationRequired = true;
		return;
	}
	else if (button1Event == ButtonEvent_Released) {
		// SAMPLE_BUTTON_1 has just been released without being held, start BLE advertising to all
		// devices.
		Log_Debug("INFO: SAMPLE_BUTTON_1 was pressed briefly, allowing new BLE bonds...\n");
		if (BleControlMessageProtocol_AllowNewBleBond(&bleAdvertiseToAllTimeoutPeriod) != 0) {
			Log_Debug("ERROR: Unable to allow new BLE bonds, check nRF52 is connected.\n");
		}
	}
	else if (button1Event == ButtonEvent_Held) {
		// When SAMPLE_BUTTON_1 is held, delete all bonded BLE devices.
		Log_Debug("INFO: SAMPLE_BUTTON_1 is held; deleting all BLE bonds...\n");
		if (BleControlMessageProtocol_DeleteAllBondedDevices() != 0) {
			Log_Debug("ERROR: Unable to delete all BLE bonds, check nRF52 is connected.\n");
		}
		else {
			Log_Debug("INFO: All BLE bonds are deleted successfully.\n");
		}
	}
	// No actions are defined for other events.

	// Take actions based on SAMPLE_BUTTON_2 events.
	ButtonEvent button2Event = GetButtonEvent(&button2State);
	if (button2Event == ButtonEvent_Error) {
		terminationRequired = true;
		return;
	}
	else if (button2Event == ButtonEvent_Released) {
		Log_Debug("INFO: SAMPLE_BUTTON_2 was pressed briefly; toggling SAMPLE_LED.\n");
		deviceControlLedState =
			(deviceControlLedState == GPIO_Value_Low ? GPIO_Value_High : GPIO_Value_Low);
		GPIO_SetValue(deviceControlLedGpioFd, deviceControlLedState);
		DeviceControlMessageProtocol_NotifyLedStatusChange();
	}
	else if (button2Event == ButtonEvent_Held) {
		// Forget all stored Wi-Fi networks
		Log_Debug("INFO: SAMPLE_BUTTON_2 is held; forgetting all stored Wi-Fi networks...\n");
		if (WifiConfig_ForgetAllNetworks() != 0) {
			Log_Debug("ERROR: Unable to forget all stored Wi-Fi networks: %s (%d).\n",
				strerror(errno), errno);
		}
		else {
			Log_Debug("INFO: All stored Wi-Fi networks are forgotten successfully.\n");
		}
	}
	// No actions are defined for other events.
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData buttonsEventData = { .eventHandler = &ButtonTimerEventHandler };

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure.</returns>
static int InitPeripheralsAndHandlers(void) {
	// Open the GPIO controlling the nRF52 reset pin, and keep it held in reset (low) until needed.
	bleDeviceResetPinGpioFd =
		GPIO_OpenAsOutput(USI_NRF52_RESET, GPIO_OutputMode_OpenDrain, GPIO_Value_Low);
	if (bleDeviceResetPinGpioFd < 0) {
		Log_Debug("ERROR: Could not open GPIO 5 as reset pin: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = TerminationHandler;
	sigaction(SIGTERM, &action, NULL);

	// Open the UART and set up UART event handler.
	UART_Config uartConfig;
	UART_InitConfig(&uartConfig);
	uartConfig.baudRate = 115200;
	uartConfig.flowControl = UART_FlowControl_RTSCTS;
	uartFd = UART_Open(USI_NRF52_UART, &uartConfig);
	if (uartFd < 0) {
		Log_Debug("ERROR: Could not open UART: %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	if (MessageProtocol_Init(epollFd, uartFd) < 0) {
		return -1;
	}

	BleControlMessageProtocol_Init(BleStateChangeHandler, epollFd);
	WifiConfigMessageProtocol_Init();
	DeviceControlMessageProtocol_Init(SetDeviceControlLedStatusHandler,
		GetDeviceControlLedStatusHandler);

	Log_Debug("Opening SAMPLE_BUTTON_1 as input\n");
	button1State.fd = GPIO_OpenAsInput(USI_BUTTON_1);
	if (button1State.fd < 0) {
		Log_Debug("ERROR: Could not open SAMPLE_BUTTON_1 GPIO: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	Log_Debug("Opening SAMPLE_BUTTON_2 as input.\n");
	button2State.fd = GPIO_OpenAsInput(USI_BUTTON_2);
	if (button2State.fd < 0) {
		Log_Debug("ERROR: Could not open SAMPLE_BUTTON_2 GPIO: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	struct timespec buttonStatusCheckPeriod = { 0, 1000000 };
	buttonTimerFd =
		CreateTimerFdAndAddToEpoll(epollFd, &buttonStatusCheckPeriod, &buttonsEventData, EPOLLIN);
	if (buttonTimerFd < 0) {
		return -1;
	}

	// Open blue RGB LED GPIO and set as output with value GPIO_Value_High (off).
	Log_Debug("Opening SAMPLE_RGBLED_BLUE\n");
	bleAdvertiseToBondedDevicesLedGpioFd =
		GPIO_OpenAsOutput(USI_BT_LED_BLUE, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (bleAdvertiseToBondedDevicesLedGpioFd < 0) {
		Log_Debug("ERROR: Could not open SAMPLE_RGBLED_BLUE GPIO: %s (%d).\n", strerror(errno),
			errno);
		return -1;
	}

	// Open red RGB LED GPIO and set as output with value GPIO_Value_High (off).
	Log_Debug("Opening SAMPLE_RGBLED_RED\n");
	bleAdvertiseToAllDevicesLedGpioFd =
		GPIO_OpenAsOutput(USI_BT_LED_RED, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (bleAdvertiseToAllDevicesLedGpioFd < 0) {
		Log_Debug("ERROR: Could not open SAMPLE_RGBLED_RED GPIO: %s (%d).\n", strerror(errno),
			errno);
		return -1;
	}

	// Open green RGB LED GPIO and set as output with value GPIO_Value_High (off).
	Log_Debug("Opening SAMPLE_RGBLED_GREEN\n");
	bleConnectedLedGpioFd =
		GPIO_OpenAsOutput(USI_BT_LED_GREEN, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (bleConnectedLedGpioFd < 0) {
		Log_Debug("ERROR: Could not open SAMPLE_RGBLED_GREEN GPIO: %s (%d).\n", strerror(errno),
			errno);
		return -1;
	}

	// Open LED GPIO and set as output with value GPIO_Value_High (off).
	Log_Debug("Opening SAMPLE_LED\n");
	deviceControlLedState = GPIO_Value_High;
	deviceControlLedGpioFd =
		GPIO_OpenAsOutput(USI_WIFI_LED_RED, GPIO_OutputMode_PushPull, deviceControlLedState);
	if (deviceControlLedGpioFd < 0) {
		Log_Debug("ERROR: Could not open SAMPLE_LED GPIO: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	UpdateBleLedStatus(BleControlMessageProtocolState_Uninitialized);

	// Initialization completed, start the nRF52 application.
	GPIO_SetValue(bleDeviceResetPinGpioFd, GPIO_Value_High);

	return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
	// Leave the LED off
	if (bleAdvertiseToBondedDevicesLedGpioFd >= 0) {
		GPIO_SetValue(bleAdvertiseToBondedDevicesLedGpioFd, GPIO_Value_High);
	}
	if (bleAdvertiseToAllDevicesLedGpioFd >= 0) {
		GPIO_SetValue(bleAdvertiseToAllDevicesLedGpioFd, GPIO_Value_High);
	}
	if (bleConnectedLedGpioFd >= 0) {
		GPIO_SetValue(bleConnectedLedGpioFd, GPIO_Value_High);
	}

	Log_Debug("Closing file descriptors\n");
	CloseFdAndPrintError(buttonTimerFd, "ButtonTimer");
	CloseFdAndPrintError(button1State.fd, "Button1");
	CloseFdAndPrintError(button2State.fd, "Button2");
	CloseFdAndPrintError(bleDeviceResetPinGpioFd, "BleDeviceResetPin");
	CloseFdAndPrintError(bleAdvertiseToBondedDevicesLedGpioFd, "BleAdvertiseToBondedDevicesLed");
	CloseFdAndPrintError(bleAdvertiseToAllDevicesLedGpioFd, "BleAdvertiseToAllDevicesLed");
	CloseFdAndPrintError(bleConnectedLedGpioFd, "BleConnectedLed");
	CloseFdAndPrintError(uartFd, "Uart");
	DeviceControlMessageProtocol_Cleanup();
	WifiConfigMessageProtocol_Cleanup();
	BleControlMessageProtocol_Cleanup();
	MessageProtocol_Cleanup();
}

int WiFiSetupByBT_Init(int wifisetupbybt_epollFd, sig_atomic_t wifisetupbybt_terminationRequired) {
	Log_Debug("INFO: BLE Wi-Fi application starting.\n");
	Log_Debug(
		"Available actions on the Azure Sphere device:\n"
		"\tPress USI_BUTTON_1  - Start allowing new BLE bonds for 1 minute\n"
		"\tHold USI_BUTTON_1   - Delete all BLE bonds\n"
		"\tPress SAMPLE_BUTTON_2  - Toggle USI_WIFI_LED_RED\n"
		"\tHold USI_BUTTON_2   - Forget all stored Wi-Fi networks on Azure Sphere device\n\n"
		"USI_BT_LED's color indicates BLE status for the attached nRF52 board:\n"
		"\tYellow  - Uninitialized\n"
		"\tBlue    - Advertising to bonded devices only\n"
		"\tRed     - Advertising to all devices\n"
		"\tGreen   - Connected to a central device\n"
		"\tMagenta - Error\n\n");

	terminationRequired = wifisetupbybt_terminationRequired;
	epollFd = wifisetupbybt_epollFd;

	if (InitPeripheralsAndHandlers() != 0) {
		return -1;
	}
	return 0;
}

void WiFiSetupByBT_Deinit(void) {
	ClosePeripheralsAndHandlers();
}