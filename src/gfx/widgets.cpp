// See widgets.h.

#include "widgets.h"

#include "../geometry.h"
#include "ink.h"
#include "palette.h"

namespace xlogin
{

void Button::draw(Canvas &c) const
{
    c.setColor(pal::kWellColor);
    c.fillRoundRect(rect, geo::kButtonRadius);

    if (filled && enabled) {
        c.setColor(accent ? pal::kAccent : pal::kWarnColor, kOnFillAlpha);
        c.fillRoundRect(rect, geo::kButtonRadius);
    }

    c.setColor(inkFor(enabled, accent), hovered ? kOutlineAlphaHover : kOutlineAlphaIdle);
    c.setPenSize(1.0f);
    c.strokeRoundRect(rect, geo::kButtonRadius);

    c.setFont(Font::Body);
    c.setFontSize(geo::kButtonTextSize);
    c.setColor(enabled ? pal::kTextColor : pal::kDisabledColor);

    // Centred horizontally on the measured width, and optically centred vertically -- a line of
    // text has more ink above its centre than below, so a geometric centre reads as too high.
    const std::string shown = c.clipToWidth(label, rect.w - 2.0f * geo::kFieldPadX);
    const float tw = c.stringWidth(shown.c_str());
    c.drawString(shown.c_str(), rect.centerX() - tw * 0.5f,
                 rect.centerY() + geo::kButtonTextSize * geo::kLabelBaselineBias);
}

//------------------------------------------------------------------------
void Checkbox::draw(Canvas &c) const
{
    c.setColor(pal::kWellColor);
    c.fillRoundRect(box, geo::kCheckRadius);
    c.setColor(inkFor(enabled, checked),
               hovered && enabled ? kOutlineAlphaHover : kOutlineAlphaIdle);
    c.setPenSize(1.0f);
    c.strokeRoundRect(box, geo::kCheckRadius);

    // The tick is drawn when checked WHETHER OR NOT the box is enabled, which is the one
    // departure from CPU-Power's checkbox. That panel never disables a checked box; this one
    // does, while the automatic login counts down, and a box that hid its tick then would say
    // automatic login was off at the moment it is about to happen. Disabled, it is drawn in
    // the disabled ink, so "disabled outranks on" still holds for the colour.
    if (checked) {
        c.setColor(enabled ? pal::kAccentBright : pal::kDisabledColor);
        c.setPenSize(2.0f);
        const float l = box.x + 3.0f, t = box.y + 3.0f, r = box.right() - 3.0f,
                    b = box.bottom() - 3.0f;
        c.strokeLine(l, (t + b) * 0.5f, (l + r) * 0.5f - 0.5f, b - 1.0f);
        c.strokeLine((l + r) * 0.5f - 0.5f, b - 1.0f, r, t);
    }

    c.setFont(Font::Body);
    c.setFontSize(geo::kCheckLabelSize);
    c.setColor(enabled ? pal::kTextColor : pal::kDisabledColor);
    c.drawString(label.c_str(), labelX, labelBaseline);
}

} // namespace xlogin
