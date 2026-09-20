// This panel's controls.
//
// Not the sibling project's widgets: see ink.h for why those could not be ported. What carries
// over is the idiom recorded there -- a kWellColor rounded fill, an optional accent wash, and a
// 1px inkFor() outline whose alpha is the entire hover treatment -- and this file applies it to
// the one control shape this window actually has.
//
// Unlike the widgets it takes its look from, a Button CARRIES ITS RECT. It is positioned from
// geometry.h by whoever lays the panel out, and it hit-tests against the same rect it draws, so
// the painter and the hit test cannot drift apart.

#pragma once

#include "canvas.h"

#include <string>

namespace xlogin
{

struct Button {
    Rect rect;
    std::string label;

    bool enabled = true;
    bool hovered = false;
    // Draws with the accent ink rather than the dim ink: this is the action the user came here
    // to take. It is not a separate style, only the `on` argument to inkFor().
    bool accent = false;
    // Adds the accent wash under the outline. Reserved for a confirmation step, where the point
    // is that the button should not look like the one that was just clicked.
    bool filled = false;

    void draw(Canvas &c) const;

    // Deliberately the drawn rect and not a padded one: every button in this panel is at least
    // 110x32 logical units, which is already a comfortable target. The sibling projects pad
    // their hit rects because their controls are 15 units tall.
    bool hit(float x, float y) const
    {
        return enabled && rect.contains(x, y);
    }
};

} // namespace xlogin
