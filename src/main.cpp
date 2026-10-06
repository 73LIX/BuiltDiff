// builtdiff - "It works on my machine" - BuiltDiff tells you why it doesn't work on yours.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "analyze.hpp"
#include "compare.hpp"
#include "json.hpp"
#include "render.hpp"
#include "snapshot.hpp"
#include "util.hpp"

namespace fs = std::filesystem;
using namespace bd;

namespace {

constexpr int kExitOk = 0, kExitMismatch = 1, kExitUsage = 2, kExitError = 3;

void usage(std::ostream& out) {
  out << "builtdiff " << kToolVersion << " - explain why a project does not build or run on this machine\n\n"
         "Developer:\n"
         "  builtdiff init                 create builtdiff.json (optional settings) in the current directory\n"
         "  builtdiff snapshot             write .builtdiff (environment fingerprint, no personal data)\n\n"
         "User:\n"
         "  builtdiff check                compare this machine with the developer's snapshot\n"
         "  builtdiff check --analyze      same, plus a plain-language explanation from a local Gemma model\n\n"
         "Options (check):\n"
         "  --analyze           explain the result with a local model (needs Ollama: https://ollama.com)\n"
         "  --model NAME        model to use (default: auto-detected from the local server, or $BUILTDIFF_MODEL)\n"
         "  --host HOST[:PORT]  Ollama server (default: $OLLAMA_HOST or 127.0.0.1:11434)\n"
         "  --allow-remote      allow a non-loopback Ollama server (the report leaves this machine!)\n"
         "  --file PATH         use this snapshot instead of searching for .builtdiff\n"
         "  --json              machine readable report on stdout\n"
         "  --verbose           also list checks that passed\n"
         "  --no-color          disable colours (also honours NO_COLOR)\n\n"
         "Exit status: 0 environment matches, 1 problems found, 2 usage error, 3 other error.\n";
}

struct Args {
  std::string cmd;
  bool analyze = false, json = false, verbose = false, no_color = false, allow_remote = false;
  std::string model, host, file;
};

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
  if (argc < 2) { err = "missing command"; return false; }
  a.cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string_view s = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) { err = std::string(s) + " needs a value"; return false; }
      dst = argv[++i];
      return true;
    };
    if (s == "--analyze") a.analyze = true;
    else if (s == "--json") a.json = true;
    else if (s == "--verbose" || s == "-v") a.verbose = true;
    else if (s == "--no-color") a.no_color = true;
    else if (s == "--allow-remote") a.allow_remote = true;
    else if (s == "--model") { if (!value(a.model)) return false; }
    else if (s == "--host") { if (!value(a.host)) return false; }
    else if (s == "--file") { if (!value(a.file)) return false; }
    else { err = "unknown option: " + sanitize_text(s, 40); return false; }
  }
  return true;
}

bool stream_is_tty(int fd) { return ::isatty(fd) != 0; }

std::string cwd_string() {
  std::error_code ec;
  auto p = fs::current_path(ec);
  return ec ? std::string(".") : p.string();
}

int cmd_init() {
  const std::string root = cwd_string();
  const std::string path = root + "/" + kConfigFile;
  if (file_exists(path)) {
    std::cerr << kConfigFile << " already exists - not overwriting it.\n";
    return kExitError;
  }
  if (!write_file_atomic(path, make_starter_config(root) + "\n")) {
    std::cerr << "error: cannot write " << path << "\n";
    return kExitError;
  }
  std::cout << "Created " << kConfigFile << ". Review it, then run: builtdiff snapshot\n";
  return kExitOk;
}

int cmd_snapshot() {
  const std::string root = cwd_string();
  std::vector<std::string> warnings;
  SnapshotOptions opts;
  opts.extra = load_config(root, warnings);
  for (const auto& w : warnings) std::cerr << "warning: " << w << "\n";
  CreateResult cr = create_snapshot(root, opts);
  for (const auto& n : cr.notes) std::cerr << "note: " << n << "\n";
  if (!cr.snap) {
    std::cerr << "error: " << cr.error << "\n";
    return kExitError;
  }
  const std::string path = root + "/" + kSnapshotFile;
  if (!write_file_atomic(path, json::dump(snapshot_to_json(*cr.snap), 2) + "\n")) {
    std::cerr << "error: cannot write " << path << "\n";
    return kExitError;
  }
  const Snapshot& s = *cr.snap;
  std::cout << "Wrote " << kSnapshotFile << " for '" << s.project << "' (" << platform_string(s.platform) << ")\n"
            << "  tools: " << s.tools.size() << ", libraries: " << s.libs.size() << ", binaries: " << s.binaries.size()
            << "\n  fingerprint: " << s.fingerprint.substr(0, 16) << "...\n"
            << "It contains no user name, home path, environment variables, IPs or private files.\n"
            << "Commit it:  git add " << kSnapshotFile << " && git commit -m \"Add builtdiff snapshot\"\n";
  return kExitOk;
}

int cmd_check(const Args& a) {
  std::string path = a.file;
  if (path.empty()) {
    auto found = find_snapshot_file(cwd_string());
    if (!found) {
      std::cerr << "error: no " << kSnapshotFile << " found here or in a parent directory.\n"
                << "Ask the project's developer to run `builtdiff snapshot` and commit the file.\n";
      return kExitError;
    }
    path = *found;
  }
  LoadResult lr = load_snapshot_file(path);
  for (const auto& w : lr.warnings) std::cerr << "warning: " << w << "\n";
  if (!lr.snap) {
    std::cerr << "error: " << lr.error << "\n";
    return kExitError;
  }
  // The project root is where we are running, not where the snapshot file sits.
  // A committed .builtdiff usually arrives from another machine (or a container
  // mount), so probing its parent directory would inspect the *developer's*
  // venv and report a false match.
  const std::string root = cwd_string();
  Report rep = compare_snapshot(*lr.snap, root);

  if (a.json) {
    std::cout << json::dump(report_to_json(rep), 2) << "\n";
    return rep.fails ? kExitMismatch : kExitOk;
  }
  RenderOptions ro;
  ro.color = !a.no_color && !std::getenv("NO_COLOR") && stream_is_tty(1);
  ro.verbose = a.verbose;
  render_report(rep, ro, std::cout);

  if (a.analyze) {
    AnalyzeOptions ao;
    ao.model = !a.model.empty() ? a.model : (std::getenv("BUILTDIFF_MODEL") ? std::getenv("BUILTDIFF_MODEL") : kDefaultModel);
    std::string host = !a.host.empty() ? a.host : (std::getenv("OLLAMA_HOST") ? std::getenv("OLLAMA_HOST") : "");
    if (!host.empty() && !parse_endpoint(host, ao.endpoint)) {
      std::cerr << "error: invalid --host / OLLAMA_HOST value\n";
      return kExitUsage;
    }
    ao.endpoint.allow_remote = a.allow_remote;

    // Model resolution: explicit flag, then environment, then ask the server
    // what it has. Only fall back to the built-in default when discovery could
    // not answer, so a machine with different tags is not stuck.
    bool explicit_model = !a.model.empty() || std::getenv("BUILTDIFF_MODEL") != nullptr;
    if (!explicit_model) {
      std::string derr;
      ao.available = list_models(ao.endpoint, ao.limits, derr);
      ao.model = select_model(ao.available, kDefaultModel);
      if (ao.model.empty()) {
        ao.model = kDefaultModel;
        if (!derr.empty())
          std::cerr << "note: could not list local models (" << sanitize_text(derr, 120)
                    << ") - trying '" << kDefaultModel << "'\n";
        else
          std::cerr << "note: no chat-capable model found on the server - trying '" << kDefaultModel << "'\n";
      } else if (derr.empty()) {
        std::cerr << "note: using detected model '" << sanitize_text(ao.model, 64) << "'\n";
      }
    }

    std::cout << "\n== Explanation (" << sanitize_text(ao.model, 64) << ", running locally) ==\n";
    std::string err;
    if (!analyze_report(rep, ao, std::cout, err))
      std::cerr << "\nnote: AI explanation unavailable - " << err << "\nThe report above is still complete.\n";
  }
  return rep.fails ? kExitMismatch : kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  if (argc >= 2) {
    std::string_view c = argv[1];
    if (c == "--help" || c == "-h" || c == "help") { usage(std::cout); return kExitOk; }
    if (c == "--version" || c == "version") { std::cout << "builtdiff " << kToolVersion << "\n"; return kExitOk; }
  }
  Args a;
  std::string err;
  if (!parse_args(argc, argv, a, err)) {
    std::cerr << "error: " << err << "\n\n";
    usage(std::cerr);
    return kExitUsage;
  }
  try {
    if (a.cmd == "init") return cmd_init();
    if (a.cmd == "snapshot") return cmd_snapshot();
    if (a.cmd == "check") return cmd_check(a);
  } catch (const std::exception& e) {
    std::cerr << "error: " << sanitize_text(e.what(), 200) << "\n";
    return kExitError;
  }
  std::cerr << "error: unknown command: " << sanitize_text(a.cmd, 40) << "\n\n";
  usage(std::cerr);
  return kExitUsage;
}
