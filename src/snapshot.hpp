// snapshot.hpp - create (developer side) and load (user side) .builtdiff files.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "model.hpp"

namespace bd {

struct ProjectConfig {
  std::vector<std::string> binaries;   // relative paths to always track
  std::vector<std::string> libraries;  // extra libraries (names)
  std::vector<std::string> tools;      // extra tools (allowlist only)
  std::string build_hint;              // e.g. "./build.sh"
};

struct SnapshotOptions {
  ProjectConfig extra;
  bool quiet = false;
};

struct CreateResult {
  std::optional<Snapshot> snap;
  std::string error;
  std::vector<std::string> notes;  // human readable remarks (skipped entries ...)
};

// Writes a starter builtdiff.json based on what auto-detection finds.
std::string make_starter_config(const std::string& root);
ProjectConfig load_config(const std::string& root, std::vector<std::string>& warnings);

std::vector<std::string> discover_binaries(const std::string& root);

CreateResult create_snapshot(const std::string& root, const SnapshotOptions& opts);

json::Value snapshot_to_json(const Snapshot& s, bool include_fingerprint = true);
std::string compute_fingerprint(const Snapshot& s);

struct LoadResult {
  std::optional<Snapshot> snap;
  std::string error;
  std::vector<std::string> warnings;
};
LoadResult parse_snapshot(std::string_view text);
LoadResult load_snapshot_file(const std::string& path);

// Looks for .builtdiff in `start` and its parents (stops at a directory that has .git).
std::optional<std::string> find_snapshot_file(const std::string& start);

}  // namespace bd
