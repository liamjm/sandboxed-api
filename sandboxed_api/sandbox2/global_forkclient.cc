// Copyright 2019 Google LLC
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

// Implementation of the sandbox2::ForkServer class.

#include "sandboxed_api/sandbox2/global_forkclient.h"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <syscall.h>
#include <unistd.h>

#include <bitset>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "sandboxed_api/util/flag.h"
#include "absl/memory/memory.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "sandboxed_api/embed_file.h"
#include "sandboxed_api/sandbox2/comms.h"
#include "sandboxed_api/sandbox2/forkserver_bin_embed.h"
#include "sandboxed_api/sandbox2/util.h"
#include "sandboxed_api/sandbox2/util/fileops.h"
#include "sandboxed_api/sandbox2/util/strerror.h"
#include "sandboxed_api/util/raw_logging.h"

namespace sandbox2 {
namespace {
enum class GlobalForkserverStartMode {
  kOnDemand,
  // MUST be the last element
  kNumGlobalForkserverStartModes,
};

class GlobalForkserverStartModeSet {
 public:
  static constexpr size_t kSize = static_cast<size_t>(
      GlobalForkserverStartMode::kNumGlobalForkserverStartModes);

  GlobalForkserverStartModeSet() {}
  explicit GlobalForkserverStartModeSet(GlobalForkserverStartMode value) {
    value_[static_cast<size_t>(value)] = true;
  }
  GlobalForkserverStartModeSet& operator|=(GlobalForkserverStartMode value) {
    value_[static_cast<size_t>(value)] = true;
    return *this;
  }
  GlobalForkserverStartModeSet operator|(
      GlobalForkserverStartMode value) const {
    GlobalForkserverStartModeSet rv(*this);
    rv |= value;
    return rv;
  }
  bool contains(GlobalForkserverStartMode value) const {
    return value_[static_cast<size_t>(value)];
  }
  bool empty() { return value_.none(); }

 private:
  std::bitset<kSize> value_;
};

bool AbslParseFlag(absl::string_view text, GlobalForkserverStartModeSet* out,
                   std::string* error) {
  *out = {};
  if (text == "never") {
    return true;
  }
  for (absl::string_view mode : absl::StrSplit(text, ',')) {
    mode = absl::StripAsciiWhitespace(mode);
    if (mode == "ondemand") {
      *out |= GlobalForkserverStartMode::kOnDemand;
    } else {
      *error = absl::StrCat("Invalid forkserver start mode: ", mode);
      return false;
    }
  }
  return true;
}

std::string ToString(GlobalForkserverStartMode mode) {
  switch (mode) {
    case GlobalForkserverStartMode::kOnDemand:
      return "ondemand";
    default:
      return "unknown";
  }
}

std::string AbslUnparseFlag(GlobalForkserverStartModeSet in) {
  std::vector<std::string> str_modes;
  for (size_t i = 0; i < GlobalForkserverStartModeSet::kSize; ++i) {
    auto mode = static_cast<GlobalForkserverStartMode>(i);
    if (in.contains(mode)) {
      str_modes.push_back(ToString(mode));
    }
  }
  if (str_modes.empty()) {
    return "never";
  }
  return absl::StrJoin(str_modes, ",");
}
bool ValidateStartMode(const char*, const std::string& flag) {
  GlobalForkserverStartModeSet unused;
  std::string error;
  if (!AbslParseFlag(flag, &unused, &error)) {
    SAPI_RAW_LOG(ERROR, "%s", error);
    return false;
  }
  return true;
}
}  // namespace
}  // namespace sandbox2

ABSL_FLAG(string, sandbox2_forkserver_start_mode, "ondemand",
          "When Sandbox2 Forkserver process should be started");
DEFINE_validator(sandbox2_forkserver_start_mode, &sandbox2::ValidateStartMode);

namespace sandbox2 {

namespace {

GlobalForkserverStartModeSet GetForkserverStartMode() {
  GlobalForkserverStartModeSet rv;
  std::string error;
  CHECK(AbslParseFlag(absl::GetFlag(FLAGS_sandbox2_forkserver_start_mode), &rv,
                      &error));
  return rv;
}

std::unique_ptr<GlobalForkClient> StartGlobalForkServer() {
  if (getenv(kForkServerDisableEnv)) {
    SAPI_RAW_VLOG(1,
                  "Start of the Global Fork-Server prevented by the '%s' "
                  "environment variable present",
                  kForkServerDisableEnv);
    return {};
  }

  if (GetForkserverStartMode().empty()) {
    SAPI_RAW_VLOG(
        1, "Start of the Global Fork-Server prevented by commandline flag");
    return {};
  }

  file_util::fileops::FDCloser exec_fd(
      sapi::EmbedFile::GetEmbedFileSingleton()->GetFdForFileToc(
          forkserver_bin_embed_create()));
  SAPI_RAW_CHECK(exec_fd.get() >= 0, "Getting FD for init binary failed");

  std::string proc_name = "S2-FORK-SERV";

  int sv[2];
  SAPI_RAW_CHECK(socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != -1,
                 "creating socket pair");

  // Fork the fork-server, and clean-up the resources (close remote sockets).
  pid_t pid = util::ForkWithFlags(SIGCHLD);
  SAPI_RAW_PCHECK(pid != -1, "during fork");

  // Child.
  if (pid == 0) {
    // Move the comms FD to the proper, expected FD number.
    // The new FD will not be CLOEXEC, which is what we want.
    dup2(sv[0], Comms::kSandbox2ClientCommsFD);

    char* const args[] = {proc_name.data(), nullptr};
    char* const envp[] = {nullptr};
    syscall(__NR_execveat, exec_fd.get(), "", args, envp, AT_EMPTY_PATH);
    SAPI_RAW_PLOG(FATAL, "Could not launch forkserver binary");
    abort();
  }

  close(sv[0]);
  return absl::make_unique<GlobalForkClient>(sv[1], pid);
}

}  // namespace

absl::Mutex GlobalForkClient::instance_mutex_(absl::kConstInit);
GlobalForkClient* GlobalForkClient::instance_ = nullptr;

void GlobalForkClient::EnsureStarted() {
  absl::MutexLock lock(&GlobalForkClient::instance_mutex_);
  EnsureStartedLocked(
      GetForkserverStartMode().contains(GlobalForkserverStartMode::kOnDemand));
}

void GlobalForkClient::Shutdown() {
  absl::MutexLock lock(&GlobalForkClient::instance_mutex_);
  delete instance_;
  instance_ = nullptr;
}

void GlobalForkClient::EnsureStartedLocked(bool start_if_needed) {
  if (!instance_ && start_if_needed) {
    instance_ = StartGlobalForkServer().release();
  }
  SAPI_RAW_CHECK(instance_ != nullptr, "global fork client not initialized");
}

pid_t GlobalForkClient::SendRequest(const ForkRequest& request, int exec_fd,
                                    int comms_fd, int user_ns_fd,
                                    pid_t* init_pid) {
  absl::MutexLock lock(&GlobalForkClient::instance_mutex_);
  EnsureStartedLocked(
      GetForkserverStartMode().contains(GlobalForkserverStartMode::kOnDemand));
  pid_t pid = instance_->fork_client_.SendRequest(request, exec_fd, comms_fd,
                                                  user_ns_fd, init_pid);
  if (instance_->comms_.IsTerminated()) {
    LOG(ERROR) << "Global forkserver connection terminated";
  }
  return pid;
}

pid_t GlobalForkClient::GetPid() {
  absl::MutexLock lock(&instance_mutex_);
  EnsureStartedLocked(
      GetForkserverStartMode().contains(GlobalForkserverStartMode::kOnDemand));
  SAPI_RAW_CHECK(instance_ != nullptr, "global fork client not initialized");
  return instance_->fork_client_.pid();
}
}  // namespace sandbox2
