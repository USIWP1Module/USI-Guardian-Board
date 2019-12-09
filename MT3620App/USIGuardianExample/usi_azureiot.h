/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include "common.h"

// Azure IoT SDK
#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>

#include "parson.h" // used to parse Device Twin messages.

#include "usi_uart.h"
#include "usi_rs232_485.h"
#include "usi_private_ethernet.h"

extern volatile sig_atomic_t terminationRequired;
extern int epollFd;

int USIAzureIoT_Init(int usiazureiot_epollFd, sig_atomic_t usiazureiot_terminationRequired, char* scopeid_str);
void USIAzureIoT_Deinit(void);
int USIAzureIoT_GetIoTStatus(void);
int USIAzureIoT_SendStringToCloud(const char *sendName, const char *sendString);