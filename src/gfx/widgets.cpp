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

} // namespace xlogin
