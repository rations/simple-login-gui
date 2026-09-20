// Finding this program's own resources (currently: the two fonts) at run time.
//
// Resolution order:
//   1. the compile-time install prefix, XLOGIN_RESOURCE_DIR_DEFAULT;
//   2. "resources" beside the executable, which is what a build tree looks like.
//
// Returns an empty string if neither contains a fonts directory. Callers treat that as "no
// bundled fonts", which FontStack already degrades to a system toy face for -- a login screen
// with the wrong typeface is usable, one that refuses to draw is not.
//
// THERE IS DELIBERATELY NO ENVIRONMENT OVERRIDE. The sibling CPU-Power window has one, and it
// is safe there for a stated reason: that process is unprivileged. This one is root and runs
// before anybody has authenticated. A substituted font is a FreeType attack surface, and an
// environment variable is the easiest thing in the world to set, so the override is not
// compiled in at all. An option that does not exist cannot be abused.

#pragma once

#include <string>

namespace xlogin
{

// Cached after the first call.
const std::string &resourceDir();

} // namespace xlogin
