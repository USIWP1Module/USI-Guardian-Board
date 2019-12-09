/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include "common.h"
#include <arpa/inet.h>
#include <applibs/networking.h>
#include "echo_tcp_server.h"

extern volatile sig_atomic_t terminationRequired;
extern int epollFd;

int USIPrivateEthernet_Init(int usieth_epollFd, sig_atomic_t usieth_terminationRequired);
void USIPrivateEthernet_Deinit(void);