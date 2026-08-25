/* lwip/sockets.h -> real Linux BSD sockets. lwip's socket API is
 * BSD-compatible, so the app's socket code maps directly. */
#pragma once
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
