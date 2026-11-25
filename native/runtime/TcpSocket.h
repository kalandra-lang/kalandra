// Native socket ownership. Include before platform headers that import windows.h.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kalandra::runtime::tcp {
#ifdef _WIN32
using Native = SOCKET;
using Length = int;
constexpr Native invalid = INVALID_SOCKET;
inline int error() { return WSAGetLastError(); }
inline void close(Native s) { closesocket(s); }
inline bool pending(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR; }
inline int initialize() {
  static const int result = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
  }();
  return result;
}
inline Native create(int af) {
  return WSASocketW(af, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
}
inline int configure(Native s) {
  if (!SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0))
    return int(GetLastError());
  u_long enable = 1;
  return ioctlsocket(s, FIONBIO, &enable) == 0 ? 0 : error();
}
inline int ready(Native s) {
  fd_set writable, failed;
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(s, &writable);
  FD_SET(s, &failed);
  timeval zero{};
  return select(0, nullptr, &writable, &failed, &zero);
}
constexpr int sendFlags = 0;
constexpr int readSide = SD_RECEIVE, writeSide = SD_SEND;
#else
using Native = int;
using Length = socklen_t;
constexpr Native invalid = -1;
inline int error() { return errno; }
inline void close(Native s) { ::close(s); }
inline bool pending(int e) {
  return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EINTR;
}
inline int initialize() { return 0; }
inline Native create(int af) {
#ifdef __linux__
  return socket(af, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
#else
  return socket(af, SOCK_STREAM, IPPROTO_TCP);
#endif
}
inline int configure(Native s) {
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
    return errno;
#endif
  if (fcntl(s, F_SETFD, FD_CLOEXEC) == -1 || fcntl(s, F_SETFL, O_NONBLOCK) == -1)
    return errno;
  return 0;
}
inline int ready(Native s) {
  pollfd descriptor{s, POLLOUT, 0};
  return ::poll(&descriptor, 1, 0);
}
#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif
constexpr int readSide = SHUT_RD, writeSide = SHUT_WR;
#endif
struct Socket {
  Native fd = invalid;
  ~Socket() {
    if (fd != invalid)
      close(fd);
  }
  Socket() = default;
  Socket(const Socket &) = delete;
  Socket(Socket &&other) noexcept : fd(std::exchange(other.fd, invalid)) {}
};
enum Direction : unsigned { Read = 1, Write = 2, Both = Read | Write };
struct Endpoint {
  std::shared_ptr<Socket> socket;
  unsigned directions = 0;
  bool listener = false;
  ~Endpoint() {
    if (!socket || listener)
      return;
    if (directions & Write)
      ::shutdown(socket->fd, writeSide);
    if (directions & Read)
      ::shutdown(socket->fd, readSide);
  }
};
using Owner = std::unique_ptr<Endpoint>;
struct Address {
  sockaddr_storage addr{};
  Length size = 0;
};
struct Addresses {
  std::array<Address, 64> candidates{};
  std::size_t count = 0, next = 0;
  std::shared_ptr<Socket> connecting;
  int lastError = 0;
};
} // namespace kalandra::runtime::tcp
