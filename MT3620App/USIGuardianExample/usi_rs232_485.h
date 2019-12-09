/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include "common.h"
#include "usi_azureiot.h"

extern volatile sig_atomic_t terminationRequired;
extern int epollFd;

int USIRs_Init(int usirs_epollFd, sig_atomic_t usirs_terminationRequired);
void USIRs_Deinit(void);
void USIRs_SendRs232Or485Msg(const char *dataToSend);