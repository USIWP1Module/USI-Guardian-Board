/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
#include <applibs/gpio.h>
#include <applibs/uart.h>
#include <applibs/log.h>
#include <applibs/wificonfig.h>
#include <applibs/networking.h>
#include <applibs/storage.h>

// This sample uses a single-thread event loop pattern, based on epoll and timerfd
#include "epoll_timerfd_utilities.h"

#include "mt3620.h"
#include "usi_mt3620_bt_combo.h"
#include "usi_mt3620_bt_guardian.h"

#define BUILD_USI_UART
#define BUILD_USI_WIFISETUPBYBT
#define BUILD_USI_PRIVATE_ETHERNET
#define BUILD_USI_RS232_485