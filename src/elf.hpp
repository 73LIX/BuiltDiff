// elf.hpp - bounds-checked ELF reader (no execution, no ldd).
//
// Extracts what is needed to explain "it doesn't run on my machine":
//   * class / endianness / machine / type
//   * PT_INTERP (dynamic loader), DT_NEEDED, DT_SONAME, DT_RPATH / DT_RUNPATH
//   * symbol-version requirements (DT_VERNEED, e.g. GLIBC_2.38, GLIBCXX_3.4.32)
//   * symbol versions a shared library defines (DT_VERDEF)
// Every read is range-checked against the mapped file, all loops are capped, so
// a malicious or truncated file produces an error string instead of UB.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bd {

struct VerNeed {
  std::string file;                  // e.g. "libc.so.6"
  std::vector<std::string> versions; // e.g. {"GLIBC_2.2.5", "GLIBC_2.38"}
};

struct ElfInfo {
  int bits = 0;           // 32 or 64
  bool little = true;
  std::uint16_t machine = 0;
  std::uint16_t type = 0; // ET_EXEC=2, ET_DYN=3
  std::string interpreter;
  std::string soname;
  std::vector<std::string> needed;
  std::vector<std::string> runpath;  // RUNPATH and RPATH entries, split on ':'
  std::vector<VerNeed> verneed;
  std::vector<std::string> verdef;   // versions defined by this object
  std::string note;                  // non-fatal parse problem (truncated tables ...)
};

enum class FileFormat { Unknown, Elf, PE, MachO, Script };

FileFormat detect_format(std::string_view head) noexcept;
const char* format_name(FileFormat f) noexcept;

// Canonical architecture name: x86_64, x86, aarch64, arm, riscv, ppc64, ...
std::string elf_arch_name(std::uint16_t e_machine, int bits, bool little);

// Parse from an in-memory image. nullopt => not a (supported) ELF file.
std::optional<ElfInfo> parse_elf(std::string_view image);

// mmap()s the file read-only (size capped) and parses it. fd stays owned by caller.
std::optional<ElfInfo> parse_elf_fd(int fd);

}  // namespace bd
