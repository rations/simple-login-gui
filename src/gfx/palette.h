// The palette, in one place.
//
// Carried over unchanged from the CPU-Power window, which took it from the rations-amp plug-in
// editor, so this login screen reads as part of the same family: the same near-black ground, the
// same gold piping, the same green accent. geometry.h re-exports every name below into `geo`, so
// layout code says geo::kAccent and there is still exactly one definition.
//
// 0xRRGGBB throughout, matching Canvas::setColor. Alpha is a separate argument and is never
// baked into a constant here.

#pragma once

#include <cstdint>

namespace xlogin
{
namespace pal
{

constexpr uint32_t kBgColor = 0x121011;   // the screen ground, behind everything
constexpr uint32_t kFaceColor = 0x1E1C1D; // the login panel sitting on it
constexpr uint32_t kWellColor = 0x0C0B0B; // sunk areas: the username and password fields
constexpr uint32_t kGold = 0xB88B4C;      // piping / hairlines
constexpr uint32_t kTextColor = 0xFFFFFF; // labels and typed text
constexpr uint32_t kDimColor = 0x9A9490;  // secondary text, placeholder text

// The accent marks the field that has focus and the button that will fire on Return. A login
// screen has exactly one such place at a time, and it has to be obvious which: a person typing
// their password into a field that does not have focus gets no feedback at all about why it is
// not working.
constexpr uint32_t kAccent = 0x3FD05A;
constexpr uint32_t kAccentBright = 0x7FE89A;

// A control that cannot be used right now -- the Log in button while PAM is working, a menu
// entry whose command is not installed -- is drawn in this and left visible, never hidden. It
// has to read as "not available", not as "absent".
constexpr uint32_t kDisabledColor = 0x4A4648;

// kWarnColor is the confirm state of a destructive menu entry: the row that is one more click
// from powering the machine off does not look like the row above it. kErrorColor is the status
// line after a failed authentication.
constexpr uint32_t kWarnColor = 0xE8A33F;
constexpr uint32_t kErrorColor = 0xFF3B30;

} // namespace pal
} // namespace xlogin
