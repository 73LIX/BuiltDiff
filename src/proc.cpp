#include "proc.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <thread>

#include "util.hpp"

extern char** environ;

namespace bd {

namespace {

using Clock = std::chrono::steady_clock;

// Make sure a descriptor is >= 3 and close-on-exec so it can never be confused
// with stdio and never leaks into the child.
int safe_fd(int fd) {
  if (fd < 0) return -1;
  int nfd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
  ::close(fd);
  return nfd;
}

bool make_pipe(int out[2]) {
  int p[2];
#ifdef __linux__
  if (::pipe2(p, O_CLOEXEC) != 0) return false;
#else
  if (::pipe(p) != 0) return false;
  ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
#endif
  out[0] = safe_fd(p[0]);
  out[1] = safe_fd(p[1]);
  if (out[0] < 0 || out[1] < 0) {
    if (out[0] >= 0) ::close(out[0]);
    if (out[1] >= 0) ::close(out[1]);
    return false;
  }
  return true;
}

std::vector<std::string> minimal_env() {
  std::vector<std::string> env;
  const char* path = std::getenv("PATH");
  env.emplace_back(std::string("PATH=") + ((path && *path) ? path : "/usr/local/bin:/usr/bin:/bin"));
  env.emplace_back("LC_ALL=C");
  env.emplace_back("LANG=C");
  for (const char* k : {"PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "HOME", "JAVA_HOME"}) {
    if (const char* v = std::getenv(k); v && *v) env.emplace_back(std::string(k) + "=" + v);
  }
  return env;
}

}  // namespace

RunResult run_capture(const std::string& exe, const std::vector<std::string>& args,
                      std::chrono::milliseconds timeout, std::size_t max_output) {
  RunResult res;
  if (exe.empty() || exe.front() != '/') return res;

  int pfd[2];
  if (!make_pipe(pfd)) return res;

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    ::close(pfd[0]);
    ::close(pfd[1]);
    return res;
  }
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
  posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);

  sigset_t none, dfl;
  sigemptyset(&none);
  sigemptyset(&dfl);
  sigaddset(&dfl, SIGPIPE);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setsigdefault(&attr, &dfl);  // we ignore SIGPIPE ourselves; children must not inherit that
  posix_spawnattr_setpgroup(&attr, 0);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

  std::vector<std::string> argv_s;
  argv_s.push_back(exe);
  argv_s.insert(argv_s.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);

  std::vector<std::string> env_s = minimal_env();
  std::vector<char*> envp;
  for (auto& e : env_s) envp.push_back(e.data());
  envp.push_back(nullptr);

  pid_t pid = -1;
  int rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  ::close(pfd[1]);
  if (rc != 0) {
    ::close(pfd[0]);
    return res;
  }
  res.spawned = true;

  const auto deadline = Clock::now() + timeout;
  std::array<char, 4096> buf;
  bool eof = false;
  while (!eof) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) { res.timed_out = true; break; }
    struct pollfd p {pfd[0], POLLIN, 0};
    int pr = ::poll(&p, 1, static_cast<int>(std::min<long long>(left, 1000)));
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    ssize_t n = ::read(pfd[0], buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      break;
    }
    if (n == 0) { eof = true; break; }
    const std::size_t room = max_output > res.output.size() ? max_output - res.output.size() : 0;
    res.output.append(buf.data(), std::min<std::size_t>(room, static_cast<std::size_t>(n)));
    if (static_cast<std::size_t>(n) > room) { res.truncated = true; break; }
  }
  ::close(pfd[0]);

  int status = 0;
  if (res.timed_out || res.truncated) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  } else {
    // stdout closed, but the process might still linger: wait with the same deadline.
    while (true) {
      pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) break;
      if (w < 0 && errno != EINTR) break;
      if (Clock::now() >= deadline) {
        res.timed_out = true;
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  if (!res.timed_out && WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
  return res;
}

std::optional<std::string> find_in_path(std::string_view name) {
  if (name.empty() || name.find('/') != std::string_view::npos || name == "." || name == "..") return std::nullopt;
  const char* path = std::getenv("PATH");
  const std::string p = (path && *path) ? path : "/usr/local/bin:/usr/bin:/bin";
  for (const std::string& dir : split(p, ':')) {
    if (dir.empty() || dir.front() != '/') continue;
    std::string full = dir;
    if (full.back() != '/') full.push_back('/');
    full.append(name);
    struct stat st {};
    if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(full.c_str(), X_OK) == 0)
      return full;
  }
  return std::nullopt;
}

}  // namespace bd
