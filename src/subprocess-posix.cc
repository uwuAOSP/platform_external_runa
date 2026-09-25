// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "subprocess.h"

#include <algorithm>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#include "util.h"

Subprocess::Subprocess(bool use_console) : fd_(-1), pid_(-1),
                                           use_console_(use_console) {
}

Subprocess::~Subprocess() {
  if (fd_ >= 0)
    close(fd_);
  // Reap child if forgotten.
  if (pid_ != -1)
    Finish();
}

bool Subprocess::Start(SubprocessSet* set, const EdgeCommand& cmd,
                       int extra_fd) {
  int output_pipe[2];
  if (pipe(output_pipe) < 0)
    Fatal("pipe: %s", strerror(errno));
  fd_ = output_pipe[0];
#if !defined(USE_PPOLL)
  // If available, we use ppoll in DoWork(); otherwise we use pselect
  // and so must avoid overly-large FDs.
  if (fd_ >= static_cast<int>(FD_SETSIZE))
    Fatal("pipe: %s", strerror(EMFILE));
#endif  // !USE_PPOLL
  SetCloseOnExec(fd_);

  posix_spawn_file_actions_t action;
  int err = posix_spawn_file_actions_init(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_init: %s", strerror(err));

  err = posix_spawn_file_actions_addclose(&action, output_pipe[0]);
  if (err != 0)
    Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));

  if (extra_fd >= 0) {
    if (posix_spawn_file_actions_adddup2(&action, extra_fd, 3) != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(errno));
  }

  posix_spawnattr_t attr;
  err = posix_spawnattr_init(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_init: %s", strerror(err));

  short flags = 0;

  flags |= POSIX_SPAWN_SETSIGMASK;
  err = posix_spawnattr_setsigmask(&attr, &set->old_mask_);
  if (err != 0)
    Fatal("posix_spawnattr_setsigmask: %s", strerror(err));
  // Signals which are set to be caught in the calling process image are set to
  // default action in the new process image, so no explicit
  // POSIX_SPAWN_SETSIGDEF parameter is needed.

  if (!use_console_) {
    // Put the child in its own process group, so ctrl-c won't reach it.
    flags |= POSIX_SPAWN_SETPGROUP;
    // No need to posix_spawnattr_setpgroup(&attr, 0), it's the default.

    // Open /dev/null over stdin.
    err = posix_spawn_file_actions_addopen(&action, 0, "/dev/null", O_RDONLY,
          0);
    if (err != 0) {
      Fatal("posix_spawn_file_actions_addopen: %s", strerror(err));
    }

    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 1);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 2);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_addclose(&action, output_pipe[1]);
    if (err != 0)
      Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));
    // In the console case, output_pipe is still inherited by the child and
    // closed when the subprocess finishes, which then notifies ninja.
  }
#ifdef POSIX_SPAWN_USEVFORK
  flags |= POSIX_SPAWN_USEVFORK;
#endif

  err = posix_spawnattr_setflags(&attr, flags);
  if (err != 0)
    Fatal("posix_spawnattr_setflags: %s", strerror(err));

  const char* spawned_args[] = { "/bin/sh", "-c", cmd.command.c_str(), NULL };
  err = posix_spawn(&pid_, "/bin/sh", &action, &attr,
        const_cast<char**>(spawned_args), cmd.env ? cmd.env : environ);
  if (err != 0)
    Fatal("posix_spawn: %s", strerror(err));

  err = posix_spawnattr_destroy(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_destroy: %s", strerror(err));
  err = posix_spawn_file_actions_destroy(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_destroy: %s", strerror(err));

  close(output_pipe[1]);
  return true;
}

void Subprocess::OnPipeReady() {
  char buf[4 << 10];
  ssize_t len = read(fd_, buf, sizeof(buf));
  if (len > 0) {
    buf_.append(buf, len);
  } else {
    if (len < 0)
      Fatal("read: %s", strerror(errno));
    close(fd_);
    fd_ = -1;
  }
}

const struct rusage* Subprocess::GetUsage() const {
  return &rusage_;
}

ExitStatus Subprocess::Finish() {
  assert(pid_ != -1);
  int status;
  if (wait4(pid_, &status, 0, &rusage_) < 0)
    Fatal("wait4(%d): %s", pid_, strerror(errno));
  pid_ = -1;

  if (WIFEXITED(status)) {
    int exit = WEXITSTATUS(status);
    if (exit == 0)
      return ExitSuccess;
  } else if (WIFSIGNALED(status)) {
    if (WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGTERM
        || WTERMSIG(status) == SIGHUP)
      return ExitInterrupted;
  }
  return ExitFailure;
}

bool Subprocess::Done() const {
  return fd_ == -1;
}

const string& Subprocess::GetOutput() const {
  return buf_;
}

int SubprocessSet::interrupted_;

void SubprocessSet::SetInterruptedFlag(int signum) {
  interrupted_ = signum;
}

void SubprocessSet::HandlePendingInterruption() {
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == -1) {
    perror("ninja: sigpending");
    return;
  }
  if (sigismember(&pending, SIGINT))
    interrupted_ = SIGINT;
  else if (sigismember(&pending, SIGTERM))
    interrupted_ = SIGTERM;
  else if (sigismember(&pending, SIGHUP))
    interrupted_ = SIGHUP;
}

SubprocessSet::SubprocessSet() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &set, &old_mask_) < 0)
    Fatal("sigprocmask: %s", strerror(errno));

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SetInterruptedFlag;
  if (sigaction(SIGINT, &act, &old_int_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &act, &old_term_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &act, &old_hup_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
}

SubprocessSet::~SubprocessSet() {
  Clear();

  if (sigaction(SIGINT, &old_int_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &old_term_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &old_hup_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigprocmask(SIG_SETMASK, &old_mask_, 0) < 0)
    Fatal("sigprocmask: %s", strerror(errno));
}

Subprocess *SubprocessSet::Add(const EdgeCommand& cmd, int extra_fd) {
  Subprocess *subprocess = new Subprocess(cmd.use_console);
  if (!subprocess->Start(this, cmd, extra_fd)) {
    delete subprocess;
    return 0;
  }
  running_.push_back(subprocess);
  return subprocess;
}

#ifdef USE_PPOLL
bool SubprocessSet::DoWork(int control_fd, bool* control_ready) {
  vector<pollfd> fds;
  nfds_t nfds = 0;

  if (control_ready)
    *control_ready = false;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    int fd = (*i)->fd_;
    if (fd < 0)
      continue;
    pollfd pfd = { fd, POLLIN | POLLPRI, 0 };
    fds.push_back(pfd);
    ++nfds;
  }

  if (control_fd >= 0) {
    pollfd pfd = { control_fd, POLLIN | POLLPRI, 0 };
    fds.push_back(pfd);
    ++nfds;
  }

  interrupted_ = 0;
  int ret = ppoll(&fds.front(), nfds, NULL, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: ppoll");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  if (control_fd >= 0 &&
      (fds.back().revents & (POLLIN | POLLPRI)) != 0 && control_ready)
    *control_ready = true;

  nfds_t cur_nfd = 0;
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    int fd = (*i)->fd_;
    if (fd < 0)
      continue;
    assert(fd == fds[cur_nfd].fd);
    if (fds[cur_nfd++].revents) {
      (*i)->OnPipeReady();
      if ((*i)->Done()) {
        finished_.push(*i);
        i = running_.erase(i);
        continue;
      }
    }
    ++i;
  }

  return IsInterrupted();
}

#else  // !defined(USE_PPOLL)
bool SubprocessSet::DoWork(int control_fd, bool* control_ready) {
  fd_set set;
  int nfds = 0;
  FD_ZERO(&set);

  if (control_ready)
    *control_ready = false;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    int fd = (*i)->fd_;
    if (fd >= 0) {
      FD_SET(fd, &set);
      if (nfds < fd+1)
        nfds = fd+1;
    }
  }
  if (control_fd >= 0) {
    FD_SET(control_fd, &set);
    if (nfds < control_fd + 1)
      nfds = control_fd + 1;
  }

  interrupted_ = 0;
  int ret = pselect(nfds, &set, 0, 0, 0, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: pselect");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  if (control_fd >= 0 && FD_ISSET(control_fd, &set) && control_ready)
    *control_ready = true;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    int fd = (*i)->fd_;
    if (fd >= 0 && FD_ISSET(fd, &set)) {
      (*i)->OnPipeReady();
      if ((*i)->Done()) {
        finished_.push(*i);
        i = running_.erase(i);
        continue;
      }
    }
    ++i;
  }

  return IsInterrupted();
}
#endif  // !defined(USE_PPOLL)

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

bool SubprocessSet::IsRunning(Subprocess* subproc) const {
  return find(running_.begin(), running_.end(), subproc) != running_.end();
}

bool SubprocessSet::TerminateForRetry(Subprocess* subproc) {
  if (!subproc || subproc->use_console_ || !IsRunning(subproc) ||
      subproc->pid_ <= 0) {
    return false;
  }
  // Every non-console action starts in its own process group. Signal the group,
  // not just /bin/sh, so ordinary compiler descendants cannot survive retry.
  if (kill(-subproc->pid_, SIGTERM) < 0 && errno != ESRCH)
    return false;
  if (kill(-subproc->pid_, SIGKILL) < 0 && errno != ESRCH)
    return false;
  return true;
}

static bool ReadProcessGroupAndRssKB(pid_t pid, pid_t* process_group,
                                     uint64_t* rss_kb) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  FILE* stat = fopen(path, "r");
  if (!stat)
    return false;
  char buffer[4096];
  size_t size = fread(buffer, 1, sizeof(buffer) - 1, stat);
  fclose(stat);
  if (size == 0)
    return false;
  buffer[size] = '\0';
  char* closing = strrchr(buffer, ')');
  if (!closing)
    return false;
  int group;
  if (sscanf(closing + 2, "%*c %*d %d", &group) != 1)
    return false;

  snprintf(path, sizeof(path), "/proc/%d/statm", static_cast<int>(pid));
  FILE* statm = fopen(path, "r");
  if (!statm)
    return false;
  unsigned long resident_pages;
  bool parsed = fscanf(statm, "%*lu %lu", &resident_pages) == 1;
  fclose(statm);
  if (!parsed)
    return false;

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return false;
  *process_group = group;
  *rss_kb = static_cast<uint64_t>(resident_pages) *
      static_cast<uint64_t>(page_size) / 1024;
  return true;
}

uint64_t SubprocessSet::ProcessGroupRssKB(Subprocess* subproc) const {
  if (!subproc || subproc->pid_ <= 0)
    return 0;
  DIR* proc = opendir("/proc");
  if (!proc)
    return 0;
  uint64_t total = 0;
  dirent* entry;
  while ((entry = readdir(proc)) != NULL) {
    char* end = NULL;
    long pid = strtol(entry->d_name, &end, 10);
    if (!end || *end != '\0' || pid <= 0)
      continue;
    pid_t group;
    uint64_t rss_kb;
    if (ReadProcessGroupAndRssKB(static_cast<pid_t>(pid), &group, &rss_kb) &&
        group == subproc->pid_) {
      total += rss_kb;
    }
  }
  closedir(proc);
  return total;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    // Since the foreground process is in our process group, it will receive
    // the interruption signal (i.e. SIGINT or SIGTERM) at the same time as us.
    if (!(*i)->use_console_)
      kill(-(*i)->pid_, interrupted_);
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
}
