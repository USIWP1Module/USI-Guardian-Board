/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include "common.h"
#include "usi_azureiot.h"

extern volatile sig_atomic_t terminationRequired;
extern int epollFd;

int USIUart_Init(int usiuart_epollFd, sig_atomic_t usiuart_terminationRequired);
void USIUart_Deinit(void);
void USIUart_SendUartMsg(const char *dataToSend);