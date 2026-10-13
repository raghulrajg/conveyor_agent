#pragma once
#include "Ethernet.h"
typedef int nsapi_error_t; typedef int nsapi_size_or_error_t; typedef unsigned nsapi_size_t;
enum { NSAPI_ERROR_OK = 0, NSAPI_ERROR_WOULD_BLOCK = -3001, NSAPI_ERROR_IS_CONNECTED = -3015 };
enum { NSAPI_SOCKET, NSAPI_KEEPALIVE, NSAPI_KEEPIDLE, NSAPI_KEEPINTVL, NSAPI_REUSEADDR };
enum nsapi_version_t { NSAPI_IPv4 };
struct SocketAddress { SocketAddress() {} SocketAddress(uint8_t*, nsapi_version_t, uint16_t) {} const char* get_ip_address() { return "1.2.3.4"; } uint16_t get_port() { return 1; } };
struct TCPSocket {
  int open(NetworkInterface*) { return 0; } void close() {} int setsockopt(int, int, void*, unsigned) { return 0; }
  void set_timeout(int) {} void set_blocking(bool) {} int bind(uint16_t) { return 0; } int listen(int) { return 0; }
  TCPSocket* accept(nsapi_error_t*) { return nullptr; } int getpeername(SocketAddress*) { return 0; }
  int connect(SocketAddress) { return 0; } int send(const void*, unsigned n) { return (int)n; } int recv(void*, unsigned) { return NSAPI_ERROR_WOULD_BLOCK; }
};
#define osPriorityNormal 0
namespace rtos {
  struct Thread { Thread(int, int, void*, const char*) {} void start(void (*)()) {} };
  struct Mutex { void lock() {} void unlock() {} };
}
namespace mbed { struct Watchdog { static Watchdog& get_instance() { static Watchdog w; return w; } void start(uint32_t) {} void kick() {} }; }
