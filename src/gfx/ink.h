// The one genuinely reusable piece of the sibling CPU-Power window's widget file, plus the
// drawing idiom its controls share, written down so this project's controls look like that
// project's without copying controls that cannot be copied.
//
// WHY THERE IS NO PORTED widgets.cpp HERE. CPU-Power's Slider, Toggle, Checkbox and Readout are
// not parameterised widgets: none of them carries a rect, and every draw() and hit() reads
// absolute constants out of that project's geometry.h -- Toggle::draw() starts with
// `Rect r(geo::kToggleX, geo::kToggleY, geo::kToggleW, geo::kToggleH)`. They are that panel's
// specific controls, and this panel has none of the same ones. Bringing them across would mean
// bringing across a geometry header describing a 420x285 window with a five-stop governor
// slider, which is not this window. So the controls are written for this panel's geometry, and
// what crosses over is this file: the state-to-colour rule, and the conventions below that make
// a control drawn here read as a sibling of one drawn there.
//
// THE IDIOM, as measured from CPU-Power's widgets.cpp rather than remembered:
//
//   * A control is a kWellColor rounded fill, then (when it is "on") the accent at alpha 190
//     over it, then a 1px outline in inkFor() at ALPHA 255 WHEN HOVERED AND 170 OTHERWISE. That
//     alpha step is the entire hover treatment -- there is no separate hover fill, and there is
//     no pressed state at all.
//   * Corner radius is 3.0 for a box, and exactly half the height for anything meant to read as
//     a capsule.
//   * Pen size is 1.0 for outlines and hairlines, 2.0 for a checkmark or a thumb ring.
//   * A tick is drawn as two strokeLine() calls, never as a glyph: a checkmark taken from the
//     body font depends on that font having one.
//   * A label's baseline sits at r.centerY() + fontSize * 0.36f.
//   * Hit rects are deliberately larger than the ink, because a 15-unit box is not a target.
//
// Canvas already pins the half-pixel offset that makes a 1px stroke land ON the boundary rather
// than straddling two pixel rows, so none of the above has to think about it.

#pragma once

#include "palette.h"

#include <cstdint>

namespace xlogin
{

// The state-to-colour rule, ported verbatim from CPU-Power's widgets.cpp.
//
// Disabled outranks on: a control that cannot be used does not also advertise what it would
// have been set to. Everything that draws a control's ink goes through here, so "disabled" can
// never be drawn in one colour by one control and another colour by the next.
constexpr uint32_t inkFor(bool enabled, bool on)
{
    if (!enabled)
        return pal::kDisabledColor;
    return on ? pal::kAccent : pal::kDimColor;
}

// The two alphas that are the whole hover treatment. Named so a control cannot quietly invent a
// third.
constexpr int kOutlineAlphaHover = 255;
constexpr int kOutlineAlphaIdle = 170;

// The accent wash laid over a control that is on, under its outline.
constexpr int kOnFillAlpha = 190;

} // namespace xlogin
