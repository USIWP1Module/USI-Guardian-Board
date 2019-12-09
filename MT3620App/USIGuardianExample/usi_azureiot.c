/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#include "usi_azureiot.h"


// Azure IoT Hub/Central defines.
#define SCOPEID_LENGTH 20
static char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in
									 // app_manifest.json, CmdArgs

static IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle = NULL;
static const int keepalivePeriodSeconds = 20;
static bool iothubAuthenticated = false;
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void *context);
static void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payload,
	size_t payloadSize, void *userContextCallback);
static void TwinReportString(const char *propertyName, const char *propertyString);
static void TwinReportBoolState(const char *propertyName, bool propertyValue);
static void ReportStatusCallback(int result, void *context);
static const char *GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason);
static const char *getAzureSphereProvisioningResultString(
	AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult);
static void SendTelemetry(const unsigned char *key, const unsigned char *value);
static void SetupAzureClient(void);

// Function to generate simulated Temperature data/telemetry
static void SendSimulatedTemperature(void);

// Initialization/Cleanup
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);

// File descriptors - initialized to invalid value

// LED
static int deviceTwinStatusLedGpioFd = -1;
static int ioTStatusLedGpioFd = -1;
static bool statusLedOn = false;

// Timer / polling
static int azureTimerFd = -1;

// Azure IoT poll periods
static const int AzureIoTDefaultPollPeriodSeconds = 5;
static const int AzureIoTMinReconnectPeriodSeconds = 60;
static const int AzureIoTMaxReconnectPeriodSeconds = 10 * 60;

static int azureIoTPollPeriodSeconds = -1;

static void AzureTimerEventHandler(EventData *eventData);

static char *sendToCloudPropertyName = "sendToCloud";
static char *sendToDevicePropertyName = "sendToDevice";

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
}

int USIAzureIoT_Init(int usiazureiot_epollFd, sig_atomic_t usiazureiot_terminationRequired, char* scopeid_str)
{
	Log_Debug("IoT Hub/Central Application starting.\n");

	if (scopeid_str != NULL) {
		Log_Debug("Setting Azure Scope ID %s\n", scopeid_str);
		strncpy(scopeId, scopeid_str, SCOPEID_LENGTH);
	}
	else {
		Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
		return -1;
	}

	terminationRequired = usiazureiot_terminationRequired;
	epollFd = usiazureiot_epollFd;

	if (InitPeripheralsAndHandlers() != 0) {
		return -1;
	}

	return 0;
}

void USIAzureIoT_Deinit() {
	ClosePeripheralsAndHandlers();
}

/// <summary>
/// Azure timer event:  Check connection status and send telemetry
/// </summary>
static void AzureTimerEventHandler(EventData *eventData)
{
	if (ConsumeTimerFdEvent(azureTimerFd) != 0) {
		terminationRequired = true;
		return;
	}

	bool isNetworkReady = false;
	if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
		if (!isNetworkReady) {
			GPIO_SetValue(ioTStatusLedGpioFd, GPIO_Value_High);
			iothubAuthenticated = false;
		}
		if (isNetworkReady && iothubAuthenticated) {
			GPIO_SetValue(ioTStatusLedGpioFd, GPIO_Value_Low);
		}
		if (isNetworkReady && !iothubAuthenticated) {
			SetupAzureClient();
		}
	}
	else {
		Log_Debug("Failed to get Network state\n");
		GPIO_SetValue(ioTStatusLedGpioFd, GPIO_Value_High);
	}

	if (iothubAuthenticated) {
		SendSimulatedTemperature();
		IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
	}
	else {
		GPIO_SetValue(ioTStatusLedGpioFd, GPIO_Value_High);
	}
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData azureEventData = { .eventHandler = &AzureTimerEventHandler };

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

	// LED WiFi Blue is used to show Device Twin settings state
	Log_Debug("Opening USI_WIFI_LED_BLUE as output\n");
	deviceTwinStatusLedGpioFd =
		GPIO_OpenAsOutput(USI_WIFI_LED_BLUE, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (deviceTwinStatusLedGpioFd < 0) {
		Log_Debug("ERROR: Could not open LED: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	// LED WiFi Green is connect to IoTHub state
	Log_Debug("Opening USI_WIFI_LED_GREEN as output\n");
	ioTStatusLedGpioFd =
		GPIO_OpenAsOutput(USI_WIFI_LED_GREEN, GPIO_OutputMode_PushPull, GPIO_Value_High);
	if (ioTStatusLedGpioFd < 0) {
		Log_Debug("ERROR: Could not open USI_WIFI_LED_GREEN: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
	struct timespec azureTelemetryPeriod = { azureIoTPollPeriodSeconds, 0 };
	azureTimerFd =
		CreateTimerFdAndAddToEpoll(epollFd, &azureTelemetryPeriod, &azureEventData, EPOLLIN);
	if (azureTimerFd < 0) {
		return -1;
	}

	return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors\n");

	// Leave the LEDs off
	if (deviceTwinStatusLedGpioFd >= 0) {
		GPIO_SetValue(deviceTwinStatusLedGpioFd, GPIO_Value_High);
	}

	// Leave the LEDs off
	if (ioTStatusLedGpioFd >= 0) {
		GPIO_SetValue(ioTStatusLedGpioFd, GPIO_Value_High);
	}

	CloseFdAndPrintError(azureTimerFd, "AzureTimer");
	CloseFdAndPrintError(deviceTwinStatusLedGpioFd, "StatusLed");
	CloseFdAndPrintError(ioTStatusLedGpioFd, "IoTStatusLed");
}

/// <summary>
///     Sets the IoT Hub authentication state for the app
///     The SAS Token expires which will set the authentication state
/// </summary>
static void HubConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result,
	IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason,
	void *userContextCallback)
{
	iothubAuthenticated = (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED);
	Log_Debug("IoT Hub Authenticated: %s\n", GetReasonString(reason));
}

/// <summary>
///     Sets up the Azure IoT Hub connection (creates the iothubClientHandle)
///     When the SAS Token for a device expires the connection needs to be recreated
///     which is why this is not simply a one time call.
/// </summary>
static void SetupAzureClient(void)
{
	if (iothubClientHandle != NULL)
		IoTHubDeviceClient_LL_Destroy(iothubClientHandle);

	AZURE_SPHERE_PROV_RETURN_VALUE provResult =
		IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning(scopeId, 10000,
			&iothubClientHandle);
	Log_Debug("IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning returned '%s'.\n",
		getAzureSphereProvisioningResultString(provResult));

	if (provResult.result != AZURE_SPHERE_PROV_RESULT_OK) {

		// If we fail to connect, reduce the polling frequency, starting at
		// AzureIoTMinReconnectPeriodSeconds and with a backoff up to
		// AzureIoTMaxReconnectPeriodSeconds
		if (azureIoTPollPeriodSeconds == AzureIoTDefaultPollPeriodSeconds) {
			azureIoTPollPeriodSeconds = AzureIoTMinReconnectPeriodSeconds;
		}
		else {
			azureIoTPollPeriodSeconds *= 2;
			if (azureIoTPollPeriodSeconds > AzureIoTMaxReconnectPeriodSeconds) {
				azureIoTPollPeriodSeconds = AzureIoTMaxReconnectPeriodSeconds;
			}
		}

		struct timespec azureTelemetryPeriod = { azureIoTPollPeriodSeconds, 0 };
		SetTimerFdToPeriod(azureTimerFd, &azureTelemetryPeriod);

		Log_Debug("ERROR: failure to create IoTHub Handle - will retry in %i seconds.\n",
			azureIoTPollPeriodSeconds);
		return;
	}

	// Successfully connected, so make sure the polling frequency is back to the default
	azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
	struct timespec azureTelemetryPeriod = { azureIoTPollPeriodSeconds, 0 };
	SetTimerFdToPeriod(azureTimerFd, &azureTelemetryPeriod);

	iothubAuthenticated = true;

	if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_KEEP_ALIVE,
		&keepalivePeriodSeconds) != IOTHUB_CLIENT_OK) {
		Log_Debug("ERROR: failure setting option \"%s\"\n", OPTION_KEEP_ALIVE);
		return;
	}

	IoTHubDeviceClient_LL_SetDeviceTwinCallback(iothubClientHandle, TwinCallback, NULL);
	IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothubClientHandle,
		HubConnectionStatusCallback, NULL);
}

/// <summary>
///     Callback invoked when a Device Twin update is received from IoT Hub.
///     Updates local state for 'showEvents' (bool).
/// </summary>
/// <param name="payload">contains the Device Twin JSON document (desired and reported)</param>
/// <param name="payloadSize">size of the Device Twin JSON document</param>
static void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payload,
	size_t payloadSize, void *userContextCallback)
{
	size_t nullTerminatedJsonSize = payloadSize + 1;
	char *nullTerminatedJsonString = (char *)malloc(nullTerminatedJsonSize);
	if (nullTerminatedJsonString == NULL) {
		Log_Debug("ERROR: Could not allocate buffer for twin update payload.\n");
		abort();
	}

	// Copy the provided buffer to a null terminated buffer.
	memcpy(nullTerminatedJsonString, payload, payloadSize);
	// Add the null terminator at the end.
	nullTerminatedJsonString[nullTerminatedJsonSize - 1] = 0;

	JSON_Value *rootProperties = NULL;
	rootProperties = json_parse_string(nullTerminatedJsonString);
	if (rootProperties == NULL) {
		Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
		goto cleanup;
	}

	JSON_Object *rootObject = json_value_get_object(rootProperties);
	JSON_Object *desiredProperties = json_object_dotget_object(rootObject, "desired");
	if (desiredProperties == NULL) {
		desiredProperties = rootObject;
	}

	// Handle the Device Twin Desired Properties here.
	JSON_Object *LEDState = json_object_dotget_object(desiredProperties, "StatusLED");
	if (LEDState != NULL) {
		statusLedOn = (bool)json_object_get_boolean(LEDState, "value");
		GPIO_SetValue(deviceTwinStatusLedGpioFd,
			(statusLedOn == true ? GPIO_Value_Low : GPIO_Value_High));
		TwinReportBoolState("StatusLED", statusLedOn);
	}

	JSON_Object *SendMsgToDevice = json_object_dotget_object(desiredProperties, sendToDevicePropertyName);
	if (SendMsgToDevice != NULL) {
#if (defined(BUILD_USI_UART))
		USIUart_SendUartMsg(json_object_get_string(SendMsgToDevice, "value"));
#endif
#if (defined(BUILD_USI_RS232_485))
		USIRs_SendRs232Or485Msg(json_object_get_string(SendMsgToDevice, "value"));
#endif
#if (defined(BUILD_USI_PRIVATE_ETHERNET))
		USIPrivateEthernet_SendMsg(json_object_get_string(SendMsgToDevice, "value"));
#endif
	}

cleanup:
	// Release the allocated memory.
	json_value_free(rootProperties);
	free(nullTerminatedJsonString);
}

/// <summary>
///     Converts the IoT Hub connection status reason to a string.
/// </summary>
static const char *GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason)
{
	static char *reasonString = "unknown reason";
	switch (reason) {
	case IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN:
		reasonString = "IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN";
		break;
	case IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED:
		reasonString = "IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED";
		break;
	case IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL:
		reasonString = "IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL";
		break;
	case IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED:
		reasonString = "IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED";
		break;
	case IOTHUB_CLIENT_CONNECTION_NO_NETWORK:
		reasonString = "IOTHUB_CLIENT_CONNECTION_NO_NETWORK";
		break;
	case IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR:
		reasonString = "IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR";
		break;
	case IOTHUB_CLIENT_CONNECTION_OK:
		reasonString = "IOTHUB_CLIENT_CONNECTION_OK";
		break;
	}
	return reasonString;
}

/// <summary>
///     Converts AZURE_SPHERE_PROV_RETURN_VALUE to a string.
/// </summary>
static const char *getAzureSphereProvisioningResultString(
	AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult)
{
	switch (provisioningResult.result) {
	case AZURE_SPHERE_PROV_RESULT_OK:
		return "AZURE_SPHERE_PROV_RESULT_OK";
	case AZURE_SPHERE_PROV_RESULT_INVALID_PARAM:
		return "AZURE_SPHERE_PROV_RESULT_INVALID_PARAM";
	case AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY:
		return "AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY";
	case AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY:
		return "AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY";
	case AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR:
		return "AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR";
	case AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR:
		return "AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR";
	default:
		return "UNKNOWN_RETURN_VALUE";
	}
}

/// <summary>
///     Sends telemetry to IoT Hub
/// </summary>
/// <param name="key">The telemetry item to update</param>
/// <param name="value">new telemetry value</param>
static void SendTelemetry(const unsigned char *key, const unsigned char *value)
{
	static char eventBuffer[100] = { 0 };
	static const char *EventMsgTemplate = "{ \"%s\": \"%s\" }";
	int len = snprintf(eventBuffer, sizeof(eventBuffer), EventMsgTemplate, key, value);
	if (len < 0)
		return;

	Log_Debug("Sending IoT Hub Message: %s\n", eventBuffer);

	IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(eventBuffer);

	if (messageHandle == 0) {
		Log_Debug("WARNING: unable to create a new IoTHubMessage\n");
		return;
	}

	if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, SendMessageCallback,
		/*&callback_param*/ 0) != IOTHUB_CLIENT_OK) {
		Log_Debug("WARNING: failed to hand over the message to IoTHubClient\n");
	}
	else {
		Log_Debug("INFO: IoTHubClient accepted the message for delivery\n");
	}

	IoTHubMessage_Destroy(messageHandle);
}

/// <summary>
///     Callback confirming message delivered to IoT Hub.
/// </summary>
/// <param name="result">Message delivery status</param>
/// <param name="context">User specified context</param>
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void *context)
{
	Log_Debug("INFO: Message received by IoT Hub. Result is: %d\n", result);
}

static void TwinReportString(const char *propertyName, const char *propertyString)
{
	if (iothubClientHandle == NULL) {
		Log_Debug("ERROR: client not initialized\n");
	}
	else {
		static char reportedPropertiesString[512] = { 0 };
		int len = snprintf(reportedPropertiesString, 512, "{\"%s\":\"%s\"}", propertyName, propertyString, 0);
		if (len < 0)
			return;

		if (IoTHubDeviceClient_LL_SendReportedState(
			iothubClientHandle, (unsigned char *)reportedPropertiesString,
			strlen(reportedPropertiesString), ReportStatusCallback, 0) != IOTHUB_CLIENT_OK) {
			Log_Debug("ERROR: failed to set reported string for '%s'.\n", reportedPropertiesString);
		}
		else {
			Log_Debug("INFO: Reported string is '%s'.\n", reportedPropertiesString);
		}
	}
}

/// <summary>
///     Creates and enqueues a report containing the name and value pair of a Device Twin reported
///     property. The report is not sent immediately, but it is sent on the next invocation of
///     IoTHubDeviceClient_LL_DoWork().
/// </summary>
/// <param name="propertyName">the IoT Hub Device Twin property name</param>
/// <param name="propertyValue">the IoT Hub Device Twin property value</param>
static void TwinReportBoolState(const char *propertyName, bool propertyValue)
{
	if (iothubClientHandle == NULL) {
		Log_Debug("ERROR: client not initialized\n");
	}
	else {
		static char reportedPropertiesString[30] = { 0 };
		int len = snprintf(reportedPropertiesString, 30, "{\"%s\":%s}", propertyName,
			(propertyValue == true ? "true" : "false"));
		if (len < 0)
			return;

		if (IoTHubDeviceClient_LL_SendReportedState(
			iothubClientHandle, (unsigned char *)reportedPropertiesString,
			strlen(reportedPropertiesString), ReportStatusCallback, 0) != IOTHUB_CLIENT_OK) {
			Log_Debug("ERROR: failed to set reported state for '%s'.\n", propertyName);
		}
		else {
			Log_Debug("INFO: Reported state for '%s' to value '%s'.\n", propertyName,
				(propertyValue == true ? "true" : "false"));
		}
	}
}

/// <summary>
///     Callback invoked when the Device Twin reported properties are accepted by IoT Hub.
/// </summary>
static void ReportStatusCallback(int result, void *context)
{
	Log_Debug("INFO: Device Twin reported properties update result: HTTP status code %d\n", result);
}

/// <summary>
///     Generates a simulated Temperature and sends to IoT Hub.
/// </summary>
void SendSimulatedTemperature(void)
{
	static float temperature = 30.0;
	float deltaTemp = (float)(rand() % 20) / 20.0f;
	if (rand() % 2 == 0) {
		temperature += deltaTemp;
	}
	else {
		temperature -= deltaTemp;
	}

	char tempBuffer[20];
	int len = snprintf(tempBuffer, 20, "%3.2f", temperature);
	if (len > 0)
		SendTelemetry("Temperature", tempBuffer);
}

int USIAzureIoT_SendStringToCloud(const char *sendName, const char *sendString) {
	TwinReportString(sendName, sendString);
	return 0;
}

int USIAzureIoT_GetIoTStatus(void) {
	return iothubAuthenticated;
}
