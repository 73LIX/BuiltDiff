// render.hpp - terminal output for a Report (plain text, optional ANSI colour).
#pragma once

#include <iosfwd>

#include "compare.hpp"

namespace bd {

struct RenderOptions {
  bool color = false;
  bool verbose = false;  // also list items that are OK
};

void render_report(const Report& r, const RenderOptions& opts, std::ostream& out);

}  // namespace bd
