#pragma once

#include "common.h"
#include "message_protocol.h"
#include "blecontrol_message_protocol.h"
#include "wificonfig_message_protocol.h"
#include "devicecontrol_message_protocol.h"

extern volatile sig_atomic_t terminationRequired;
extern int epollFd;

int WiFiSetupByBT_Init(int wifisetupbybt_epollFd, sig_atomic_t terminationRequired);
void WiFiSetupByBT_Deinit(void);