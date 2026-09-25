/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RUNA_RUNTIME_CONTROL_H_
#define RUNA_RUNTIME_CONTROL_H_

#include <string>
using namespace std;

/// A minimal, local runtime control endpoint.
///
/// On POSIX hosts this is a Unix stream socket. Each connection carries one
/// newline-terminated request and receives one newline-terminated response.
/// Requests are consumed synchronously by the build thread so they cannot race
/// graph state transitions.
struct RuntimeControl {
  RuntimeControl();
  ~RuntimeControl();

  bool Open(const string& path, string* err);
  int fd() const { return fd_; }
  bool HasPendingRequest() const;

  bool Receive(string* request, string* err);
  void Reply(const string& response);

 private:
  int fd_;
  int client_fd_;
  string path_;
};

#endif  // RUNA_RUNTIME_CONTROL_H_
