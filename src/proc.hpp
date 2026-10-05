// proc.hpp - run an external program safely.
//
//  * never goes through a shell (posix_spawn with an argv vector)
//  * executable must be an absolute path (use find_in_path first)
//  * stdin is /dev/null, stdout+stderr are captured through one pipe
//  * hard wall-clock timeout and output cap; child runs in its own process
//    group so the whole group can be killed
//  * minimal, explicit environment (PATH, LC_ALL=C, a few pkg-config vars)
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bd {

struct RunResult {
  bool spawned = false;
  int exit_code = -1;  // -1 if killed by a signal or not spawned
  bool timed_out = false;
  bool truncated = false;
  std::string output;
};

RunResult run_capture(const std::string& exe_abs_path, const std::vector<std::string>& args,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(4000),
                      std::size_t max_output = 16 * 1024);

// Looks `name` up in $PATH. Ignores empty / relative PATH entries (a "." in PATH
// must never make us run something from the project directory).
// `name` must be a plain file name (no '/').
std::optional<std::string> find_in_path(std::string_view name);

}  // namespace bd
