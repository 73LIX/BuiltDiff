#include "elf.hpp"

#include <sys/mman.h>
#include <sys/stat.h>

#include <array>
#include <bit>
#include <cstring>

#include "util.hpp"

namespace bd {

namespace {

constexpr std::uint64_t kMaxMapped = 1ull << 30;  // 1 GiB
constexpr std::size_t kMaxStr = 4096;
constexpr std::size_t kMaxPhdrs = 256;
constexpr std::size_t kMaxDyn = 4096;
constexpr std::size_t kMaxNeeded = 512;
constexpr std::size_t kMaxVer = 8192;

// Tags
constexpr std::int64_t DT_NULL = 0, DT_NEEDED = 1, DT_STRTAB = 5, DT_STRSZ = 10, DT_SONAME = 14,
                       DT_RPATH = 15, DT_RUNPATH = 29, DT_VERDEF = 0x6ffffffc,
                       DT_VERDEFNUM = 0x6ffffffd, DT_VERNEED = 0x6ffffffe, DT_VERNEEDNUM = 0x6fffffff;
constexpr std::uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3;

class Reader {
 public:
  Reader(std::string_view d, bool little) : d_(d), swap_(little != (std::endian::native == std::endian::little)) {}

  bool ok(std::uint64_t off, std::uint64_t n) const noexcept {
    return off <= d_.size() && n <= d_.size() - off;
  }

  template <class T>
  bool rd(std::uint64_t off, T& out) const noexcept {
    if (!ok(off, sizeof(T))) return false;
    std::array<unsigned char, sizeof(T)> b;
    std::memcpy(b.data(), d_.data() + off, sizeof(T));
    if (swap_) for (std::size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(b[i], b[sizeof(T) - 1 - i]);
    std::memcpy(&out, b.data(), sizeof(T));
    return true;
  }

  // NUL-terminated string at off, confined to [off, limit).
  std::optional<std::string> cstr(std::uint64_t off, std::uint64_t limit, std::size_t maxlen = kMaxStr) const {
    if (limit > d_.size()) limit = d_.size();
    if (off >= limit) return std::nullopt;
    std::uint64_t end = std::min<std::uint64_t>(limit, off + maxlen);
    for (std::uint64_t i = off; i < end; ++i) {
      if (d_[i] == '\0') return std::string(d_.substr(off, i - off));
    }
    return std::nullopt;  // unterminated within bounds
  }

 private:
  std::string_view d_;
  bool swap_;
};

struct Load {
  std::uint64_t vaddr, off, filesz;
};

std::optional<std::uint64_t> vaddr_to_off(const std::vector<Load>& loads, std::uint64_t va) {
  for (const auto& l : loads) {
    if (va >= l.vaddr && va - l.vaddr < l.filesz) return l.off + (va - l.vaddr);
  }
  return std::nullopt;
}

void add_paths(std::vector<std::string>& out, const std::string& s) {
  for (auto& p : split(s, ':')) {
    if (out.size() >= 64) break;
    out.push_back(std::move(p));
  }
}

}  // namespace

FileFormat detect_format(std::string_view h) noexcept {
  if (h.size() >= 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return FileFormat::Elf;
  if (h.size() >= 2 && h[0] == 'M' && h[1] == 'Z') return FileFormat::PE;
  if (h.size() >= 2 && h[0] == '#' && h[1] == '!') return FileFormat::Script;
  if (h.size() >= 4) {
    std::uint32_t m;
    std::memcpy(&m, h.data(), 4);
    if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || m == 0xCEFAEDFEu || m == 0xCFFAEDFEu) return FileFormat::MachO;
  }
  return FileFormat::Unknown;
}

const char* format_name(FileFormat f) noexcept {
  switch (f) {
    case FileFormat::Elf: return "elf";
    case FileFormat::PE: return "pe";
    case FileFormat::MachO: return "macho";
    case FileFormat::Script: return "script";
    default: return "unknown";
  }
}

std::string elf_arch_name(std::uint16_t m, int bits, bool little) {
  switch (m) {
    case 3: return "x86";
    case 62: return "x86_64";
    case 40: return "arm";
    case 183: return "aarch64";
    case 243: return bits == 64 ? "riscv64" : "riscv32";
    case 20: return "ppc";
    case 21: return little ? "ppc64le" : "ppc64";
    case 22: return "s390x";
    case 8: return "mips";
    case 258: return "loongarch64";
    default: return "machine-" + std::to_string(m);
  }
}

std::optional<ElfInfo> parse_elf(std::string_view img) {
  if (img.size() < 64 || detect_format(img) != FileFormat::Elf) return std::nullopt;
  const unsigned char cls = static_cast<unsigned char>(img[4]);
  const unsigned char data = static_cast<unsigned char>(img[5]);
  if ((cls != 1 && cls != 2) || (data != 1 && data != 2)) return std::nullopt;

  ElfInfo info;
  info.bits = cls == 2 ? 64 : 32;
  info.little = data == 1;
  const bool is64 = info.bits == 64;
  Reader r(img, info.little);

  std::uint64_t phoff = 0;
  std::uint16_t phentsize = 0, phnum = 0;
  if (!r.rd(16, info.type) || !r.rd(18, info.machine)) return std::nullopt;
  if (is64) {
    if (!r.rd(32, phoff) || !r.rd(54, phentsize) || !r.rd(56, phnum)) return std::nullopt;
  } else {
    std::uint32_t po = 0;
    if (!r.rd(28, po) || !r.rd(42, phentsize) || !r.rd(44, phnum)) return std::nullopt;
    phoff = po;
  }
  const std::size_t min_ph = is64 ? 56 : 32;
  if (phnum == 0 || phentsize < min_ph) return info;  // static / object file without headers
  if (phnum > kMaxPhdrs) {
    info.note = "too many program headers";
    return info;
  }

  std::vector<Load> loads;
  std::uint64_t dyn_off = 0, dyn_size = 0;
  bool have_dyn = false;
  for (std::size_t i = 0; i < phnum; ++i) {
    const std::uint64_t base = phoff + static_cast<std::uint64_t>(i) * phentsize;
    std::uint32_t p_type = 0;
    std::uint64_t p_off = 0, p_vaddr = 0, p_filesz = 0;
    if (!r.rd(base, p_type)) { info.note = "truncated program headers"; break; }
    if (is64) {
      if (!r.rd(base + 8, p_off) || !r.rd(base + 16, p_vaddr) || !r.rd(base + 32, p_filesz)) {
        info.note = "truncated program headers";
        break;
      }
    } else {
      std::uint32_t a = 0, b = 0, c = 0;
      if (!r.rd(base + 4, a) || !r.rd(base + 8, b) || !r.rd(base + 16, c)) {
        info.note = "truncated program headers";
        break;
      }
      p_off = a; p_vaddr = b; p_filesz = c;
    }
    if (p_type == PT_LOAD) {
      if (loads.size() < kMaxPhdrs) loads.push_back({p_vaddr, p_off, p_filesz});
    } else if (p_type == PT_INTERP) {
      if (auto s = r.cstr(p_off, p_off + std::min<std::uint64_t>(p_filesz, 512), 512)) info.interpreter = *s;
    } else if (p_type == PT_DYNAMIC) {
      dyn_off = p_off; dyn_size = p_filesz; have_dyn = true;
    }
  }
  if (!have_dyn) return info;

  // ---- dynamic section
  const std::size_t dyn_ent = is64 ? 16 : 8;
  std::uint64_t strtab_va = 0, strsz = 0, verneed_va = 0, verneednum = 0, verdef_va = 0, verdefnum = 0;
  bool have_strtab = false;
  std::vector<std::uint64_t> needed_off;
  std::uint64_t soname_off = 0, rpath_off = 0, runpath_off = 0;
  bool have_soname = false, have_rpath = false, have_runpath = false;

  const std::uint64_t ndyn = std::min<std::uint64_t>(dyn_size / dyn_ent, kMaxDyn);
  for (std::uint64_t i = 0; i < ndyn; ++i) {
    const std::uint64_t at = dyn_off + i * dyn_ent;
    std::int64_t tag = 0;
    std::uint64_t val = 0;
    if (is64) {
      if (!r.rd(at, tag) || !r.rd(at + 8, val)) { info.note = "truncated dynamic section"; break; }
    } else {
      std::int32_t t = 0;
      std::uint32_t v = 0;
      if (!r.rd(at, t) || !r.rd(at + 4, v)) { info.note = "truncated dynamic section"; break; }
      tag = t; val = v;
    }
    if (tag == DT_NULL) break;
    switch (tag) {
      case DT_NEEDED: if (needed_off.size() < kMaxNeeded) needed_off.push_back(val); break;
      case DT_STRTAB: strtab_va = val; have_strtab = true; break;
      case DT_STRSZ: strsz = val; break;
      case DT_SONAME: soname_off = val; have_soname = true; break;
      case DT_RPATH: rpath_off = val; have_rpath = true; break;
      case DT_RUNPATH: runpath_off = val; have_runpath = true; break;
      case DT_VERNEED: verneed_va = val; break;
      case DT_VERNEEDNUM: verneednum = val; break;
      case DT_VERDEF: verdef_va = val; break;
      case DT_VERDEFNUM: verdefnum = val; break;
      default: break;
    }
  }
  if (!have_strtab) return info;
  auto st_off = vaddr_to_off(loads, strtab_va);
  if (!st_off) { info.note = "dynamic string table not mapped"; return info; }
  const std::uint64_t st_limit = strsz ? *st_off + strsz : img.size();
  auto dstr = [&](std::uint64_t o) -> std::optional<std::string> {
    if (o >= (strsz ? strsz : img.size())) return std::nullopt;
    return r.cstr(*st_off + o, st_limit);
  };

  for (auto o : needed_off)
    if (auto s = dstr(o)) info.needed.push_back(std::move(*s));
  if (have_soname) if (auto s = dstr(soname_off)) info.soname = *s;
  if (have_rpath) if (auto s = dstr(rpath_off)) add_paths(info.runpath, *s);
  if (have_runpath) if (auto s = dstr(runpath_off)) add_paths(info.runpath, *s);

  // ---- version requirements
  if (verneed_va && verneednum) {
    if (auto base = vaddr_to_off(loads, verneed_va)) {
      std::uint64_t p = *base;
      std::size_t total_aux = 0;
      const std::uint64_t cnt = std::min<std::uint64_t>(verneednum, 512);
      for (std::uint64_t i = 0; i < cnt; ++i) {
        std::uint16_t vn_cnt = 0;
        std::uint32_t vn_file = 0, vn_aux = 0, vn_next = 0;
        if (!r.rd(p + 2, vn_cnt) || !r.rd(p + 4, vn_file) || !r.rd(p + 8, vn_aux) || !r.rd(p + 12, vn_next)) {
          info.note = "truncated version-needed table";
          break;
        }
        VerNeed vn;
        if (auto f = dstr(vn_file)) vn.file = *f;
        std::uint64_t ap = p + vn_aux;
        for (std::uint16_t k = 0; k < vn_cnt && total_aux < kMaxVer; ++k, ++total_aux) {
          std::uint32_t name = 0, next = 0;
          if (!r.rd(ap + 8, name) || !r.rd(ap + 12, next)) { info.note = "truncated version-needed aux"; break; }
          if (auto s = dstr(name)) vn.versions.push_back(std::move(*s));
          if (next == 0) break;
          ap += next;
        }
        if (!vn.file.empty()) info.verneed.push_back(std::move(vn));
        if (vn_next == 0) break;
        p += vn_next;
      }
    }
  }

  // ---- version definitions (what a shared library provides)
  if (verdef_va && verdefnum) {
    if (auto base = vaddr_to_off(loads, verdef_va)) {
      std::uint64_t p = *base;
      const std::uint64_t cnt = std::min<std::uint64_t>(verdefnum, kMaxVer);
      for (std::uint64_t i = 0; i < cnt; ++i) {
        std::uint16_t vd_flags = 0;
        std::uint32_t vd_aux = 0, vd_next = 0;
        if (!r.rd(p + 2, vd_flags) || !r.rd(p + 12, vd_aux) || !r.rd(p + 16, vd_next)) {
          info.note = "truncated version-definition table";
          break;
        }
        if (!(vd_flags & 1u)) {  // skip VER_FLG_BASE (the file's own soname entry)
          std::uint32_t name = 0;
          if (r.rd(p + vd_aux, name))
            if (auto s = dstr(name)) info.verdef.push_back(std::move(*s));
        }
        if (vd_next == 0) break;
        p += vd_next;
      }
    }
  }
  return info;
}

std::optional<ElfInfo> parse_elf_fd(int fd) {
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
  if (st.st_size < 64 || static_cast<std::uint64_t>(st.st_size) > kMaxMapped) return std::nullopt;
  const auto size = static_cast<std::size_t>(st.st_size);
  void* m = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (m == MAP_FAILED) return std::nullopt;
  auto info = parse_elf(std::string_view(static_cast<const char*>(m), size));
  ::munmap(m, size);
  return info;
}

}  // namespace bd
