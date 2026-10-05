#include "render.hpp"

#include <ostream>
#include <set>

#include "util.hpp"

namespace bd {

namespace {

struct Style {
  bool on;
  const char* wrap(Status s, const char* text, std::string& buf) const {
    if (!on) return text;
    const char* code = "0";
    switch (s) {
      case Status::Fail: code = "1;31"; break;
      case Status::Warn: code = "1;33"; break;
      case Status::Info: code = "36"; break;
      case Status::Ok: code = "32"; break;
    }
    buf = std::string("\x1b[") + code + "m" + text + "\x1b[0m";
    return buf.c_str();
  }
};

const char* tag(Status s) {
  switch (s) {
    case Status::Fail: return "FAIL";
    case Status::Warn: return "WARN";
    case Status::Info: return "INFO";
    case Status::Ok: return " OK ";
  }
  return "????";
}

void indent_lines(std::ostream& out, const std::string& text, const char* prefix) {
  for (const auto& line : split(text, '\n'))
    if (!trim(line).empty()) out << prefix << sanitize_text(line, 300) << "\n";
}

}  // namespace

void render_report(const Report& r, const RenderOptions& opts, std::ostream& out) {
  Style st{opts.color};
  std::string buf;
  out << "BuiltDiff - " << (r.project.empty() ? "project" : r.project) << "\n";
  out << "This machine: " << platform_string(r.local) << "\n";

  std::string current_layer;
  for (const auto& it : r.items) {
    if (it.status == Status::Ok && !opts.verbose) continue;
    if (it.layer != current_layer) {
      current_layer = it.layer;
      out << "\n== " << layer_title(it.layer) << " ==\n";
    }
    out << "[" << st.wrap(it.status, tag(it.status), buf) << "] " << it.title;
    if (!it.name.empty() && it.name != it.title) out << " (" << it.name << ")";
    out << "\n";
    if (!it.dev.empty() || !it.you.empty()) {
      out << "       developer: " << (it.dev.empty() ? "-" : it.dev) << "\n";
      out << "       you:       " << (it.you.empty() ? "-" : it.you) << "\n";
    }
    if (!it.detail.empty()) indent_lines(out, it.detail, "       ");
    if (!it.hint.empty()) out << "       hint: " << it.hint << "\n";
  }

  out << "\n";
  if (r.fails == 0 && r.warns == 0) {
    out << st.wrap(Status::Ok, "Your environment matches the developer's.", buf) << " (" << r.oks << " checks passed";
    if (r.infos) out << ", " << r.infos << " notes";
    out << ")\n";
  } else {
    out << r.fails << " failure(s), " << r.warns << " warning(s), " << r.infos << " note(s), " << r.oks << " ok\n";
  }
  if (!r.install_cmd.empty()) out << "\nTo install what is missing:\n  " << r.install_cmd << "\n";
}

}  // namespace bd
