// Loading and drawing the optional background image.
//
// PNG and JPEG. Cairo reads PNG natively; it does not read JPEG, which is the entire reason
// libjpeg is linked. Verified in cairo.h, not assumed.
//
// THIS RUNS AS ROOT BEFORE ANYBODY HAS AUTHENTICATED, so the whole file is written around one
// idea: an image decoder is a parser, and the only safe parser input is one that only root
// could have supplied. Concretely --
//
//   * Images come from ONE directory, opened once with O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
//     and every candidate is opened relative to that directory fd. There is no path from the
//     UI to this code; a caller can name a file IN the directory and nothing else.
//   * A candidate is used only if fstat says it is a regular file, owned by uid 0, and not
//     group- or world-writable. Anything else is refused with one warning.
//   * The whole file is read into memory first, capped, and the decoder sees a byte buffer.
//     Nothing decodes straight from a path, so there is no window between the check and the
//     read for the file to be swapped.
//   * The format is decided by SNIFFING THE MAGIC BYTES, never by the filename extension.
//   * Dimensions are capped before a pixel is allocated.
//   * libjpeg's default error handler calls exit(). It is replaced, because a malformed file
//     must degrade to "no background", never to a dark tty1.
//
// Every failure returns null with exactly one warning on stderr, and every caller treats null
// as "draw the flat ground instead".

#pragma once

#include "canvas.h"

#include <cairo/cairo.h>

#include <string>
#include <vector>

namespace xlogin
{

// How an image is fitted to the screen. `Fill` is the default: scale to cover, centre, and let
// the overflow crop, which is what a wallpaper is normally expected to do.
enum class BgMode { Fill, Fit, Center, Stretch, Tile };

// Parses the XLOGIN_BG_MODE value. Unrecognised input gives Fill, with no warning -- the config
// file is root-owned and a typo there should not stop the login screen appearing.
BgMode bgModeFromString(const std::string &s);
const char *bgModeName(BgMode mode);

// The names of the candidate images in `dir`, sorted. This is a CHEAP PRE-FILTER, not a
// guarantee: it applies the ownership checks in full, but it only sniffs each file's first
// eight bytes, so a file that is truncated after its header, or whose header declares a size
// over the cap, still appears here and then fails to load. Listing a directory must not mean
// decoding every image in it, so the caller handles a failed load on selection and says so.
// Empty on any error.
std::vector<std::string> listBackgroundImages(const std::string &dir);

// Loads `dir`/`name`. `name` must be a plain filename: anything containing a '/' is refused, so
// a caller cannot escape the directory even if it wanted to. Returns null on any failure.
// The caller owns the returned surface and destroys it with cairo_surface_destroy.
cairo_surface_t *loadBackgroundImage(const std::string &dir, const std::string &name);

// Paints `image` over `dest` according to `mode`. A null image is a no-op, so the ground drawn
// underneath shows through and a failed load costs nothing.
void drawBackground(Canvas &c, cairo_surface_t *image, const Rect &dest, BgMode mode);

// Caps, exposed so the layout audit and the tests can use the same numbers.
// 8192 is generous for a screen and still bounds the allocation at 8192*8192*4 = 256 MiB, which
// is refused rather than attempted. 64 MiB bounds the read itself.
constexpr int kMaxImageDimension = 8192;
constexpr long kMaxImageFileBytes = 64L * 1024L * 1024L;

} // namespace xlogin
