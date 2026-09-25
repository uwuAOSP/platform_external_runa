/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_control.h"

#include <algorithm>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "util.h"

RuntimeControl::RuntimeControl() : fd_(-1), client_fd_(-1) {}

RuntimeControl::~RuntimeControl() {
#ifndef _WIN32
  if (client_fd_ >= 0)
    close(client_fd_);
  if (fd_ >= 0)
    close(fd_);
  if (!path_.empty())
    unlink(path_.c_str());
#endif
}

bool RuntimeControl::Open(const string& path, string* err) {
  if (path.empty())
    return true;
  if (fd_ >= 0) {
    if (path == path_)
      return true;
    *err = "control socket is already open";
    return false;
  }
#ifdef _WIN32
  *err = "runtime control is not supported on Windows";
  return false;
#else
  fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_ < 0) {
    *err = strerror(errno);
    return false;
  }
  SetCloseOnExec(fd_);
#if !defined(USE_PPOLL)
  if (fd_ >= FD_SETSIZE) {
    *err = "control socket descriptor exceeds FD_SETSIZE";
    close(fd_);
    fd_ = -1;
    return false;
  }
#endif

  sockaddr_un address = {};
  if (path.size() >= sizeof(address.sun_path)) {
    *err = "control socket path is too long";
    close(fd_);
    fd_ = -1;
    return false;
  }
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    *err = "binding control socket: " + string(strerror(errno));
    close(fd_);
    fd_ = -1;
    return false;
  }
  path_ = path;
  if (chmod(path.c_str(), 0600) < 0) {
    *err = "setting control socket permissions: " + string(strerror(errno));
    return false;
  }
  if (listen(fd_, 4) < 0) {
    *err = "listening on control socket: " + string(strerror(errno));
    return false;
  }
  return true;
#endif
}

bool RuntimeControl::Receive(string* request, string* err) {
  request->clear();
#ifdef _WIN32
  *err = "runtime control is not supported on Windows";
  return false;
#else
  if (client_fd_ >= 0) {
    close(client_fd_);
    client_fd_ = -1;
  }
  client_fd_ = accept(fd_, NULL, NULL);
  if (client_fd_ < 0) {
    *err = strerror(errno);
    return false;
  }
  SetCloseOnExec(client_fd_);
#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  setsockopt(client_fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
             sizeof(no_sigpipe));
#endif

  timeval timeout = {};
  timeout.tv_sec = 1;
  setsockopt(client_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  const size_t kMaxRequestSize = 4096;
  char buffer[512];
  while (request->size() < kMaxRequestSize) {
    size_t capacity = kMaxRequestSize - request->size();
    ssize_t size = read(client_fd_, buffer,
                        std::min(sizeof(buffer), capacity));
    if (size < 0 && errno == EINTR)
      continue;
    if (size <= 0) {
      *err = size == 0 ? "unterminated control request" : strerror(errno);
      return false;
    }
    request->append(buffer, size);
    size_t newline = request->find_first_of("\r\n");
    if (newline != string::npos) {
      request->resize(newline);
      if (request->empty()) {
        *err = "empty control request";
        return false;
      }
      return true;
    }
  }
  *err = "control request is too long";
  return false;
#endif
}

bool RuntimeControl::HasPendingRequest() const {
#ifdef _WIN32
  return false;
#else
  if (fd_ < 0)
    return false;
  pollfd descriptor = { fd_, POLLIN | POLLPRI, 0 };
  int result;
  do {
    result = poll(&descriptor, 1, 0);
  } while (result < 0 && errno == EINTR);
  return result > 0 &&
      (descriptor.revents & (POLLIN | POLLPRI)) != 0;
#endif
}

void RuntimeControl::Reply(const string& response) {
#ifdef _WIN32
  (void)response;
#else
  if (client_fd_ < 0)
    return;
  string message = response + "\n";
  const char* data = message.data();
  size_t remaining = message.size();
  while (remaining > 0) {
#ifdef MSG_NOSIGNAL
    ssize_t written = send(client_fd_, data, remaining, MSG_NOSIGNAL);
#else
    ssize_t written = write(client_fd_, data, remaining);
#endif
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      break;
    data += written;
    remaining -= written;
  }
  close(client_fd_);
  client_fd_ = -1;
#endif
}
