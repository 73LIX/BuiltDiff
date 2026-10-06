#include "knowledge.hpp"

#include <algorithm>
#include <array>
#include <filesystem>

#include "util.hpp"

namespace fs = std::filesystem;

namespace bd {

// ------------------------------------------------------------------ tools

const std::vector<ToolDef>& all_tool_defs() noexcept {
  static const std::vector<ToolDef> defs = {
      {"g++", "compiler", "cxx", {"--version"}},
      {"gcc", "compiler", "cc", {"--version"}},
      {"clang++", "compiler", "cxx", {"--version"}},
      {"clang", "compiler", "cc", {"--version"}},
      {"ld", "linker", "", {"--version"}},
      {"make", "build", "", {"--version"}},
      {"ninja", "build", "", {"--version"}},
      {"cmake", "build", "", {"--version"}},
      {"meson", "build", "", {"--version"}},
      {"pkg-config", "pkgconfig", "", {"--version"}},
      {"python3", "interpreter", "", {"--version"}},
      {"node", "interpreter", "", {"--version"}},
      {"npm", "build", "", {"--version"}},
      {"java", "interpreter", "", {"-version"}},
      {"javac", "compiler", "", {"-version"}},
      {"mvn", "build", "", {"-version"}},
      {"gradle", "build", "", {"--version"}},
      {"rustc", "compiler", "", {"--version"}},
      {"cargo", "build", "", {"--version"}},
      {"go", "compiler", "", {"version"}},
      {"git", "vcs", "", {"--version"}},
  };
  return defs;
}

const ToolDef* find_tool_def(std::string_view name) noexcept {
  for (const auto& d : all_tool_defs())
    if (name == d.name) return &d;
  return nullptr;
}

// -------------------------------------------------------------- libraries

namespace {
struct Alias {
  const char* from;  // lowercase
  const char* to;    // space separated pkg-config modules
};
constexpr Alias kAliases[] = {
    {"sdl2", "sdl2"},
    {"sdl3", "sdl3"},
    {"sdl2_image", "SDL2_image"},
    {"sdl2_ttf", "SDL2_ttf"},
    {"sdl2_mixer", "SDL2_mixer"},
    {"curl", "libcurl"},
    {"png", "libpng"},
    {"freetype", "freetype2"},
    {"gtk3", "gtk+-3.0"},
    {"gtk2", "gtk+-2.0"},
    {"opengl", "gl"},
    {"glfw", "glfw3"},
    {"ffmpeg", "libavcodec libavformat libavutil"},
    {"avcodec", "libavcodec"},
    {"opencv", "opencv4 opencv"},
    {"qt5", "Qt5Core"},
    {"qt6", "Qt6Core"},
    {"qt5widgets", "Qt5Widgets"},
    {"qt6widgets", "Qt6Widgets"},
    {"wayland", "wayland-client"},
    {"libxml2", "libxml-2.0"},
    {"xml2", "libxml-2.0"},
    {"pcre2", "libpcre2-8"},
    {"lz4", "liblz4"},
    {"zstd", "libzstd"},
    {"lzma", "liblzma"},
    {"jpeg", "libjpeg"},
    {"tiff", "libtiff-4"},
    {"webp", "libwebp"},
    {"glib", "glib-2.0"},
    {"glib2", "glib-2.0"},
    {"dbus", "dbus-1"},
    {"postgresql", "libpq"},
    {"portaudio", "portaudio-2.0"},
    {"pulseaudio", "libpulse"},
    {"sndfile", "sndfile"},
    {"alsa", "alsa"},
    {"x11", "x11"},
    {"sqlite", "sqlite3"},
    {"gtkmm", "gtkmm-3.0"},
    {"lua", "lua"},
    {"ssl", "openssl"},
};

constexpr const char* kIgnoredCmake[] = {
    "threads", "pkgconfig", "cmakefinddependencymacro", "doxygen", "git", "perl", "sphinx",
    "openmp", "swig", "python", "python3", "interpreter", "filesystem", "ccache", "clangformat",
};
constexpr const char* kBaseLibs[] = {"m", "pthread", "dl", "rt", "c", "stdc++", "util", "gcc", "gcc_s",
                                     "crypt", "resolv", "atomic", "c++", "c++fs", "stdc++fs"};
}  // namespace

std::vector<std::string> pkgconfig_candidates(std::string_view name) {
  std::vector<std::string> out;
  auto add = [&](std::string s) {
    if (s.empty()) return;
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
  };
  const std::string lower = to_lower(name);
  if (name.find('-') != std::string_view::npos || name.find('+') != std::string_view::npos ||
      name.find('.') != std::string_view::npos)
    add(std::string(name));  // already looks like a pkg-config module name
  for (const auto& a : kAliases) {
    if (lower == a.from)
      for (auto& m : split(a.to, ' ')) add(m);
  }
  add(lower);
  add(std::string(name));
  return out;
}

bool is_ignored_cmake_package(std::string_view name) noexcept {
  const std::string lower = to_lower(name);
  for (const char* i : kIgnoredCmake)
    if (lower == i) return true;
  return false;
}

bool is_base_link_lib(std::string_view name) noexcept {
  for (const char* b : kBaseLibs)
    if (name == b) return true;
  return false;
}

// ----------------------------------------------------------- distro data

PkgMgr detect_pkg_mgr(std::string_view id, std::string_view like) {
  const std::string i = to_lower(id);
  const std::string l = " " + to_lower(like) + " ";
  auto in = [&](std::initializer_list<const char*> ids) {
    for (const char* x : ids)
      if (i == x) return true;
    return false;
  };
  auto like_has = [&](const char* x) { return l.find(std::string(" ") + x + " ") != std::string::npos; };
  if (in({"arch", "manjaro", "endeavouros", "cachyos", "artix", "garuda", "arcolinux"}) || like_has("arch"))
    return PkgMgr::Pacman;
  if (in({"debian", "ubuntu", "linuxmint", "pop", "raspbian", "kali", "zorin", "elementary", "neon"}) ||
      like_has("debian") || like_has("ubuntu"))
    return PkgMgr::Apt;
  if (in({"fedora", "rhel", "centos", "rocky", "almalinux", "nobara"}) || like_has("fedora") ||
      like_has("rhel"))
    return PkgMgr::Dnf;
  return PkgMgr::None;
}

const char* pkg_mgr_name(PkgMgr m) noexcept {
  switch (m) {
    case PkgMgr::Pacman: return "pacman";
    case PkgMgr::Apt: return "apt";
    case PkgMgr::Dnf: return "dnf";
    default: return "unknown";
  }
}

std::string install_command(PkgMgr m, const std::vector<std::string>& pkgs) {
  if (pkgs.empty()) return {};
  std::string cmd;
  switch (m) {
    case PkgMgr::Pacman: cmd = "sudo pacman -S --needed"; break;
    case PkgMgr::Apt: cmd = "sudo apt install"; break;
    case PkgMgr::Dnf: cmd = "sudo dnf install"; break;
    default: return {};
  }
  for (const auto& p : pkgs) cmd += " " + p;
  return cmd;
}

namespace {
struct Row {
  const char* keys;  // comma separated, lowercase
  const char* pacman;
  const char* apt;
  const char* dnf;
};

constexpr Row kToolPkgs[] = {
    {"gcc", "gcc", "gcc", "gcc"},
    {"g++", "gcc", "g++", "gcc-c++"},
    {"clang,clang++", "clang", "clang", "clang"},
    {"ld", "binutils", "binutils", "binutils"},
    {"make", "make", "make", "make"},
    {"ninja", "ninja", "ninja-build", "ninja-build"},
    {"cmake", "cmake", "cmake", "cmake"},
    {"meson", "meson", "meson", "meson"},
    {"pkg-config", "pkgconf", "pkg-config", "pkgconf-pkg-config"},
    {"python3", "python", "python3", "python3"},
    {"node", "nodejs", "nodejs", "nodejs"},
    {"npm", "npm", "npm", "npm"},
    {"java", "jre-openjdk", "default-jre", "java-latest-openjdk"},
    {"javac", "jdk-openjdk", "default-jdk", "java-latest-openjdk-devel"},
    {"mvn", "maven", "maven", "maven"},
    {"gradle", "gradle", "gradle", "gradle"},
    {"rustc,cargo", "rust", "rustc cargo", "rust cargo"},
    {"go", "go", "golang-go", "golang"},
    {"git", "git", "git", "git"},
};

constexpr Row kLibPkgs[] = {
    {"sdl2", "sdl2", "libsdl2-dev", "SDL2-devel"},
    {"sdl3", "sdl3", "libsdl3-dev", "SDL3-devel"},
    {"sdl2_image", "sdl2_image", "libsdl2-image-dev", "SDL2_image-devel"},
    {"sdl2_ttf", "sdl2_ttf", "libsdl2-ttf-dev", "SDL2_ttf-devel"},
    {"sdl2_mixer", "sdl2_mixer", "libsdl2-mixer-dev", "SDL2_mixer-devel"},
    {"openssl", "openssl", "libssl-dev", "openssl-devel"},
    {"zlib", "zlib", "zlib1g-dev", "zlib-devel"},
    {"libcurl", "curl", "libcurl4-openssl-dev", "libcurl-devel"},
    {"libpng", "libpng", "libpng-dev", "libpng-devel"},
    {"freetype2", "freetype2", "libfreetype-dev", "freetype-devel"},
    {"gtk+-3.0", "gtk3", "libgtk-3-dev", "gtk3-devel"},
    {"gtk4", "gtk4", "libgtk-4-dev", "gtk4-devel"},
    {"vulkan", "vulkan-headers vulkan-icd-loader", "libvulkan-dev", "vulkan-headers vulkan-loader-devel"},
    {"gl", "libglvnd", "libgl-dev", "libglvnd-devel"},
    {"glew", "glew", "libglew-dev", "glew-devel"},
    {"glfw3", "glfw", "libglfw3-dev", "glfw-devel"},
    {"sqlite3", "sqlite", "libsqlite3-dev", "sqlite-devel"},
    {"fmt", "fmt", "libfmt-dev", "fmt-devel"},
    {"spdlog", "spdlog", "libspdlog-dev", "spdlog-devel"},
    {"boost", "boost", "libboost-all-dev", "boost-devel"},
    {"protobuf", "protobuf", "libprotobuf-dev protobuf-compiler", "protobuf-devel"},
    {"opencv4,opencv", "opencv", "libopencv-dev", "opencv-devel"},
    {"eigen3", "eigen", "libeigen3-dev", "eigen3-devel"},
    {"qt5core,qt5widgets", "qt5-base", "qtbase5-dev", "qt5-qtbase-devel"},
    {"qt6core,qt6widgets", "qt6-base", "qt6-base-dev", "qt6-qtbase-devel"},
    {"alsa", "alsa-lib", "libasound2-dev", "alsa-lib-devel"},
    {"x11", "libx11", "libx11-dev", "libX11-devel"},
    {"wayland-client", "wayland", "libwayland-dev", "wayland-devel"},
    {"openal", "openal", "libopenal-dev", "openal-soft-devel"},
    {"libuv", "libuv", "libuv1-dev", "libuv-devel"},
    {"yaml-cpp", "yaml-cpp", "libyaml-cpp-dev", "yaml-cpp-devel"},
    {"nlohmann_json", "nlohmann-json", "nlohmann-json3-dev", "json-devel"},
    {"gtest", "gtest", "libgtest-dev", "gtest-devel"},
    {"jsoncpp", "jsoncpp", "libjsoncpp-dev", "jsoncpp-devel"},
    {"libxml-2.0", "libxml2", "libxml2-dev", "libxml2-devel"},
    {"libpcre2-8", "pcre2", "libpcre2-dev", "pcre2-devel"},
    {"libzstd", "zstd", "libzstd-dev", "libzstd-devel"},
    {"liblz4", "lz4", "liblz4-dev", "lz4-devel"},
    {"libavcodec,libavformat,libavutil", "ffmpeg", "libavcodec-dev libavformat-dev libavutil-dev", "ffmpeg-devel"},
    {"glib-2.0", "glib2", "libglib2.0-dev", "glib2-devel"},
    {"ncurses", "ncurses", "libncurses-dev", "ncurses-devel"},
    {"readline", "readline", "libreadline-dev", "readline-devel"},
    {"libpq", "postgresql-libs", "libpq-dev", "libpq-devel"},
    {"portaudio-2.0", "portaudio", "portaudio19-dev", "portaudio-devel"},
    {"libpulse", "libpulse", "libpulse-dev", "pulseaudio-libs-devel"},
    {"sndfile", "libsndfile", "libsndfile1-dev", "libsndfile-devel"},
    {"libjpeg", "libjpeg-turbo", "libjpeg-dev", "libjpeg-turbo-devel"},
    {"libtiff-4", "libtiff", "libtiff-dev", "libtiff-devel"},
    {"libwebp", "libwebp", "libwebp-dev", "libwebp-devel"},
    {"cairo", "cairo", "libcairo2-dev", "cairo-devel"},
    {"pango", "pango", "libpango1.0-dev", "pango-devel"},
    {"harfbuzz", "harfbuzz", "libharfbuzz-dev", "harfbuzz-devel"},
};

// soname prefix -> packages (blank = unknown for that manager; use find_owner_hint)
constexpr Row kSonamePkgs[] = {
    {"libSDL2-", "sdl2", "", "SDL2"},
    {"libSDL2_image-", "sdl2_image", "", "SDL2_image"},
    {"libSDL2_ttf-", "sdl2_ttf", "", "SDL2_ttf"},
    {"libSDL2_mixer-", "sdl2_mixer", "", "SDL2_mixer"},
    {"libssl.so", "openssl", "", "openssl-libs"},
    {"libcrypto.so", "openssl", "", "openssl-libs"},
    {"libz.so", "zlib", "zlib1g", "zlib"},
    {"libstdc++.so", "gcc-libs", "libstdc++6", "libstdc++"},
    {"libgcc_s.so", "gcc-libs", "libgcc-s1", "libgcc"},
    {"libc.so", "glibc", "libc6", "glibc"},
    {"libm.so", "glibc", "libc6", "glibc"},
    {"libdl.so", "glibc", "libc6", "glibc"},
    {"libpthread.so", "glibc", "libc6", "glibc"},
    {"ld-linux", "glibc", "libc6", "glibc"},
    {"libcurl.so", "curl", "", "libcurl"},
    {"libpng16.so", "libpng", "", "libpng"},
    {"libfreetype.so", "freetype2", "", "freetype"},
    {"libGL.so", "libglvnd", "", "libglvnd-glx"},
    {"libGLX.so", "libglvnd", "", "libglvnd-glx"},
    {"libOpenGL.so", "libglvnd", "", "libglvnd-opengl"},
    {"libEGL.so", "libglvnd", "", "libglvnd-egl"},
    {"libvulkan.so", "vulkan-icd-loader", "", "vulkan-loader"},
    {"libX11.so", "libx11", "", "libX11"},
    {"libasound.so", "alsa-lib", "", "alsa-lib"},
    {"libsqlite3.so", "sqlite", "", "sqlite-libs"},
    {"libfmt.so", "fmt", "", "fmt"},
    {"libspdlog.so", "spdlog", "", "spdlog"},
    {"libgtk-3.so", "gtk3", "", "gtk3"},
    {"libglfw.so", "glfw", "", "glfw"},
    {"libGLEW.so", "glew", "", "glew"},
    {"libopenal.so", "openal", "", "openal-soft"},
    {"libwayland-client.so", "wayland", "", "libwayland-client"},
    {"libyaml-cpp.so", "yaml-cpp", "", "yaml-cpp"},
    {"libprotobuf.so", "protobuf", "", "protobuf"},
    {"libuv.so", "libuv", "", "libuv"},
    {"libjpeg.so", "libjpeg-turbo", "", "libjpeg-turbo"},
    {"libpulse.so", "libpulse", "", "pulseaudio-libs"},
    {"libxml2.so", "libxml2", "", "libxml2"},
    {"libzstd.so", "zstd", "", "libzstd"},
    {"liblz4.so", "lz4", "", "lz4-libs"},
    {"libpcre2-8.so", "pcre2", "", "pcre2"},
    {"libavcodec.so", "ffmpeg", "", ""},
    {"libavformat.so", "ffmpeg", "", ""},
    {"libavutil.so", "ffmpeg", "", ""},
    {"libswscale.so", "ffmpeg", "", ""},
    {"libswresample.so", "ffmpeg", "", ""},
    {"libboost_", "boost-libs", "", "boost"},
    {"libQt5Core.so", "qt5-base", "", "qt5-qtbase"},
    {"libQt6Core.so", "qt6-base", "", "qt6-qtbase"},
};

const char* pick(const Row& r, PkgMgr m) {
  switch (m) {
    case PkgMgr::Pacman: return r.pacman;
    case PkgMgr::Apt: return r.apt;
    case PkgMgr::Dnf: return r.dnf;
    default: return "";
  }
}

bool key_matches(const char* keys, std::string_view wanted) {
  for (const auto& k : split(keys, ','))
    if (k == wanted) return true;
  return false;
}
}  // namespace

std::vector<std::string> packages_for_tool(PkgMgr m, std::string_view tool) {
  for (const auto& r : kToolPkgs)
    if (key_matches(r.keys, tool)) return split(pick(r, m), ' ');
  return {};
}

std::vector<std::string> packages_for_library(PkgMgr m, std::string_view name) {
  for (const auto& cand : pkgconfig_candidates(name)) {
    const std::string lc = to_lower(cand);
    for (const auto& r : kLibPkgs)
      if (key_matches(r.keys, lc)) return split(pick(r, m), ' ');
  }
  return {};
}

std::vector<std::string> packages_for_soname(PkgMgr m, std::string_view soname) {
  for (const auto& r : kSonamePkgs) {
    if (soname.substr(0, std::string_view(r.keys).size()) == r.keys) return split(pick(r, m), ' ');
  }
  return {};
}

std::string find_owner_hint(PkgMgr m, std::string_view soname) {
  const std::string s(soname);
  switch (m) {
    case PkgMgr::Pacman: return "pacman -F " + s + "   (run 'sudo pacman -Fy' once to refresh the file database)";
    case PkgMgr::Apt: return "apt-file search " + s + "   (needs: sudo apt install apt-file && sudo apt-file update)";
    case PkgMgr::Dnf: return "dnf provides '*/" + s + "'";
    default: return "search your distribution's package index for the file " + s;
  }
}

std::optional<StdRequirement> cxx_std_requirement(std::string_view n) noexcept {
  if (n == "17") return StdRequirement{8, 7};
  if (n == "20") return StdRequirement{11, 14};
  if (n == "23") return StdRequirement{13, 17};
  if (n == "26" || n == "2c") return StdRequirement{14, 19};
  return std::nullopt;
}

// ------------------------------------------------------- language packages

bool project_has_venv(const std::string& root) {
  if (root.empty()) return false;
  std::error_code ec;
  for (const char* rel : {".venv", "venv", "env"}) {
    if (fs::exists(root + "/" + rel + "/pyvenv.cfg", ec)) return true;
  }
  return false;
}

std::string eco_install_command(Eco eco, const std::vector<std::string>& names, const std::string& project_root) {
  if (names.empty()) return {};
  // Re-validate every name. These strings came out of a snapshot file, which is
  // untrusted input: the command below is only ever printed, but a name with a
  // space or a quote would still make the printed line wrong and misleading.
  std::string list;
  for (const auto& n : names) {
    const bool ok = (eco == Eco::Maven) ? is_maven_coordinate(n) : is_eco_package_name(eco, n);
    if (!ok) continue;
    if (!list.empty()) list += " ";
    list += sanitize_text(n, 64);
  }
  if (list.empty()) return {};

  // Prefer the project's own virtualenv when it has one: the developer activated
  // it, so that is where the package belongs. An activated VIRTUAL_ENV that is
  // not in the project is left alone - we must not guess at paths outside it.
  const bool venv = project_has_venv(project_root);
  switch (eco) {
    case Eco::Pip:
      return (venv ? "pip install " : "python3 -m pip install ") + list;
    case Eco::Npm:
      return "npm install " + list;
    case Eco::Cargo:
      return "cargo add " + list;  // note: writes Cargo.toml; see the hint text
    case Eco::Maven:
      return "mvn dependency:get -Dartifact=" + list;
    case Eco::Go:
      return "go get " + list;
    case Eco::Native:
      break;
  }
  return {};
}

std::string eco_install_hint(Eco eco, std::string_view name, const std::string& project_root) {
  const std::string pkg(name);
  const bool ok = (eco == Eco::Maven) ? is_maven_coordinate(pkg) : is_eco_package_name(eco, pkg);
  if (!ok) return {};
  // The pinned version is what the developer actually used, so suggest exactly
  // that rather than the range: it is reproducible and it always exists.
  const std::string cmd = eco_install_command(eco, {pkg}, project_root);
  if (cmd.empty()) return {};
  switch (eco) {
    case Eco::Cargo:
      return cmd + "   (edits Cargo.toml; use --version to pick a version)";
    case Eco::Maven:
      return cmd + "   (downloads into ~/.m2; most projects should add it to pom.xml instead)";
    case Eco::Go:
      return cmd + "   (adds it to go.mod)";
    case Eco::Pip:
      if (project_has_venv(project_root))
        return cmd + "   (installs into this project's .venv)";
      return cmd + "   (system-wide; prefer python3 -m venv .venv if this project has none)";
    case Eco::Npm:
      return cmd + "   (adds it to package.json)";
    case Eco::Native:
      break;
  }
  return cmd;
}

}  // namespace bd
