/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#define _GNU_SOURCE // required for asprintf
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>

#include <sys/socket.h>

#include <applibs/log.h>

#include "echo_tcp_server.h"

EchoServer_ServerState *eth_ServerState = NULL;
static char *sendToCloudPropertyName = "sendToCloud";

// Support functions.
static void HandleListenEvent(EventData *eventData);
static void LaunchRead(EchoServer_ServerState *serverState);
static void HandleClientReadEvent(EventData *eventData);
static void LaunchWrite(EchoServer_ServerState *serverState);
static void HandleClientWriteEvent(EventData *eventData);
static int OpenIpV4Socket(in_addr_t ipAddr, uint16_t port, int sockType);
static void ReportError(const char *desc);
static void StopServer(EchoServer_ServerState *serverState, EchoServer_StopReason reason);
static EchoServer_ServerState *EventDataToServerState(EventData *eventData, size_t offset);

EchoServer_ServerState *EchoServer_Start(int epollFd, in_addr_t ipAddr, uint16_t port,
                                         int backlogSize,
                                         void (*shutdownCallback)(EchoServer_StopReason))
{
    EchoServer_ServerState *serverState = malloc(sizeof(*serverState));
    if (!serverState) {
        abort();
    }

    // Set EchoServer_ServerState state to unused values so it can be safely cleaned up if only a
    // subset of the resources are successfully allocated.
    serverState->epollFd = epollFd;
    serverState->listenFd = -1;
    serverState->clientFd = -1;
    serverState->listenEvent.eventHandler = HandleListenEvent;
    serverState->clientReadEvent.eventHandler = HandleClientReadEvent;
    serverState->epollInEnabled = false;
    serverState->clientWriteEvent.eventHandler = HandleClientWriteEvent;
    serverState->epollOutEnabled = false;
    serverState->txPayload = NULL;
    serverState->shutdownCallback = shutdownCallback;

    int sockType = SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK;
    serverState->listenFd = OpenIpV4Socket(ipAddr, port, sockType);
    if (serverState->listenFd < 0) {
        ReportError("open socket");
        goto fail;
    }

    // Be notified asynchronously when a client connects.
    RegisterEventHandlerToEpoll(epollFd, serverState->listenFd, &serverState->listenEvent, EPOLLIN);

    int result = listen(serverState->listenFd, backlogSize);
    if (result != 0) {
        ReportError("listen");
        goto fail;
    }

    Log_Debug("INFO: TCP server: Listening for client connection (fd %d).\n",
              serverState->listenFd);

    return serverState;

fail:
    EchoServer_ShutDown(serverState);
    return NULL;
}

void EchoServer_ShutDown(EchoServer_ServerState *serverState)
{
    if (!serverState) {
        return;
    }

    CloseFdAndPrintError(serverState->clientFd, "clientFd");
    CloseFdAndPrintError(serverState->listenFd, "listenFd");

    free(serverState->txPayload);

    free(serverState);
}

static void HandleListenEvent(EventData *eventData)
{
    EchoServer_ServerState *serverState =
        EventDataToServerState(eventData, offsetof(EchoServer_ServerState, listenEvent));
    int localFd = -1;

    do {
        // Create a new accepted socket to connect to the client.
        // The newly-accepted sockets should be opened in non-blocking mode, and use
        // EPOLLIN and EPOLLOUT to transfer data.
        struct sockaddr in_addr;
        socklen_t sockLen = sizeof(in_addr);
        localFd = accept4(serverState->listenFd, &in_addr, &sockLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (localFd < 0) {
            ReportError("accept");
            break;
        }

        Log_Debug("INFO: TCP server: Accepted client connection (fd %d).\n", localFd);

        if (serverState->clientFd >= 0) {
			Log_Debug("INFO: TCP server: Get new connection, unregister old client.\n");
			UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->clientFd);
			serverState->epollInEnabled = false;
        }

        // Socket opened successfully, so transfer ownership to EchoServer_ServerState object.
        serverState->clientFd = localFd;
        localFd = -1;

        LaunchRead(serverState);
		eth_ServerState = serverState;
    } while (0);

    close(localFd);
}

static void LaunchRead(EchoServer_ServerState *serverState)
{
    serverState->inLineSize = 0;
    RegisterEventHandlerToEpoll(serverState->epollFd, serverState->clientFd,
                                &serverState->clientReadEvent, EPOLLIN);
}

static void HandleClientReadEvent(EventData *eventData)
{
    EchoServer_ServerState *serverState =
        EventDataToServerState(eventData, offsetof(EchoServer_ServerState, clientReadEvent));

    if (serverState->epollInEnabled) {
        UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->clientFd);
        serverState->epollInEnabled = false;
    }

    // Continue until no immediately available input or until an error occurs.
    size_t maxChars = sizeof(serverState->input) - 1;

    while (true) {
        // Read a single byte from the client and add it to the buffered line.
        uint8_t b;
        ssize_t bytesReadOneSysCall = recv(serverState->clientFd, &b, 1, /* flags */ 0);

        // If successfully read a single byte then process it.
        if (bytesReadOneSysCall == 1) {
            // If received newline then print received line to debug log.
            if (b == '\r') {
                serverState->input[serverState->inLineSize] = '\0';
                Log_Debug("INFO: TCP server: Received \"%s\"\n", serverState->input);
                LaunchRead(serverState);

				//When received newline will send message to Azure IoT
				USIAzureIoT_SendStringToCloud(sendToCloudPropertyName, serverState->input);
                break;
            }

            // If new character is not printable then discard.
            else if (!isprint(b)) {
                // Special case '\n' to avoid printing a message for every line of input.
                if (b != '\n') {
                    Log_Debug("INFO: TCP server: Discarding unprintable character 0x%02x\n", b);
                }
            }

            // If new character would leave no space for NUL terminator then reset buffer.
            else if (serverState->inLineSize == maxChars) {
                Log_Debug("INFO: TCP server: Input data overflow. Discarding %zu characters.\n",
                          maxChars);
                serverState->input[0] = b;
                serverState->inLineSize = 1;
            }

            // Else append character to buffer.
            else {
                serverState->input[serverState->inLineSize] = b;
                ++serverState->inLineSize;
            }
        }

        // If client has shut down cleanly then terminate.
        else if (bytesReadOneSysCall == 0) {
            Log_Debug("INFO: TCP server: Client has closed connection, so terminating server.\n");
            //StopServer(serverState, EchoServer_StopReason_ClientClosed);
            break;
        }

        // If receive buffer is empty then wait for EPOLLIN event.
        else if (bytesReadOneSysCall == -1 && errno == EAGAIN) {
            RegisterEventHandlerToEpoll(serverState->epollFd, serverState->clientFd,
                                        &serverState->clientReadEvent, EPOLLIN);
            serverState->epollInEnabled = true;
			LaunchRead(serverState);
            break;
        }

        // Another error occured so abort the program.
        else {
            ReportError("recv");
            StopServer(serverState, EchoServer_StopReason_Error);
            break;
        }
    }
}

static void LaunchWrite(EchoServer_ServerState *serverState)
{
    // Allocate a client response on the heap.
    char *str;
    int result = asprintf(&str, "Received \"%s\"\r\n", serverState->input);
    if (result == -1) {
        ReportError("asprintf");
        StopServer(serverState, EchoServer_StopReason_Error);
        return;
    }

    // Start to send the response.
    serverState->txPayloadSize = (size_t)result;
    serverState->txPayload = (uint8_t *)str;
    serverState->txBytesSent = 0;
    HandleClientWriteEvent(&serverState->clientWriteEvent);
}

static void HandleClientWriteEvent(EventData *eventData)
{
    EchoServer_ServerState *serverState =
        EventDataToServerState(eventData, offsetof(EchoServer_ServerState, clientWriteEvent));

    if (serverState->epollOutEnabled) {
        UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->clientFd);
        serverState->epollOutEnabled = false;
    }

    // Continue until have written entire response, error occurs, or OS TX buffer is full.
    while (serverState->txBytesSent < serverState->txPayloadSize) {
        size_t remainingBytes = serverState->txPayloadSize - serverState->txBytesSent;
        const uint8_t *data = &serverState->txPayload[serverState->txBytesSent];
        ssize_t bytesSentOneSysCall =
            send(serverState->clientFd, data, remainingBytes, /* flags */ 0);

        // If successfully sent data then stay in loop and try to send more data.
        if (bytesSentOneSysCall > 0) {
            serverState->txBytesSent += (size_t)bytesSentOneSysCall;
        }

        // If OS TX buffer is full then wait for next EPOLLOUT.
        else if (bytesSentOneSysCall < 0 && errno == EAGAIN) {
            RegisterEventHandlerToEpoll(serverState->epollFd, serverState->clientFd,
                                        &serverState->clientWriteEvent, EPOLLOUT);
            serverState->epollOutEnabled = true;
            return;
        }

        // Another error occurred so terminate the program.
        else {
            ReportError("send");
            StopServer(serverState, EchoServer_StopReason_Error);
            return;
        }
    }

    // If reached here then successfully sent entire payload so clean up and read next line from
    // client.
    free(serverState->txPayload);
    serverState->txPayload = NULL;

    LaunchRead(serverState);
}

static int OpenIpV4Socket(in_addr_t ipAddr, uint16_t port, int sockType)
{
    int localFd = -1;
    int retFd = -1;

    do {
        // Create a TCP / IPv4 socket. This will form the listen socket.
        localFd = socket(AF_INET, sockType, /* protocol */ 0);
        if (localFd < 0) {
            ReportError("socket");
            break;
        }

        // Enable rebinding soon after a socket has been closed.
        int enableReuseAddr = 1;
        int r = setsockopt(localFd, SOL_SOCKET, SO_REUSEADDR, &enableReuseAddr,
                           sizeof(enableReuseAddr));
        if (r != 0) {
            ReportError("setsockopt/SO_REUSEADDR");
            break;
        }

        // Bind to a well-known IP address.
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ipAddr;
        addr.sin_port = htons(port);

        r = bind(localFd, (const struct sockaddr *)&addr, sizeof(addr));
        if (r != 0) {
            ReportError("bind");
            break;
        }

        // Port opened successfully.
        retFd = localFd;
        localFd = -1;
    } while (0);

    close(localFd);

    return retFd;
}

static void ReportError(const char *desc)
{
    Log_Debug("ERROR: TCP server: \"%s\", errno=%d (%s)\n", desc, errno, strerror(errno));
}

static void StopServer(EchoServer_ServerState *serverState, EchoServer_StopReason reason)
{
    // Stop listening for incoming connections.
    if (serverState->listenFd != -1) {
        UnregisterEventHandlerFromEpoll(serverState->epollFd, serverState->listenFd);
    }

    serverState->shutdownCallback(reason);
}

static EchoServer_ServerState *EventDataToServerState(EventData *eventData, size_t offset)
{
    uint8_t *eventData8 = (uint8_t *)eventData;
    uint8_t *serverState8 = eventData8 - offset;
    return (EchoServer_ServerState *)serverState8;
}

void USIPrivateEthernet_SendMsg(const char *dataToSend)
{
	// Allocate a client response on the heap.
	char *str;
	int result = asprintf(&str, "%s\r", dataToSend);
	if (result == -1) {
		ReportError("asprintf");
		StopServer(eth_ServerState, EchoServer_StopReason_Error);
		return;
	}

	if (eth_ServerState != NULL) {
		// Start to send the response.
		eth_ServerState->txPayloadSize = (size_t)result;
		eth_ServerState->txPayload = (uint8_t *)str;
		eth_ServerState->txBytesSent = 0;
		HandleClientWriteEvent(&eth_ServerState->clientWriteEvent);
	}
	else {
		Log_Debug("Could not found client\n");
	}
}