// See menu.h.

#include "menu.h"

#include "../geometry.h"
#include "ink.h"
#include "palette.h"

#include <cmath>

namespace xlogin
{

namespace
{

inline float snap(float v)
{
    return std::floor(v + 0.5f);
}

} // namespace

//------------------------------------------------------------------------
void Menu::setItems(std::vector<MenuItem> items)
{
    mItems = std::move(items);
    mArmed = -1;
    mHover = -1;
    mHighlight = 0;
}

//------------------------------------------------------------------------
void Menu::open(const Rect &anchor, const Rect &screen)
{
    const float h = geo::menuHeight(static_cast<int>(mItems.size()));

    // Above the button by preference: the button is in the panel's bottom row, so a menu
    // dropping downwards would go off the bottom of a small screen.
    float x = snap(anchor.left());
    float y = snap(anchor.top() - geo::kMenuGap - h);

    // If there is not room above, flip below. If there is not room either way -- a screen
    // shorter than the menu -- clamp, because a menu drawn off the edge of a login screen is
    // an escape route that is not there.
    if (y < screen.top())
        y = snap(anchor.bottom() + geo::kMenuGap);
    if (y + h > screen.bottom())
        y = snap(screen.bottom() - h);
    if (y < screen.top())
        y = snap(screen.top());

    if (x + geo::kMenuW > screen.right())
        x = snap(screen.right() - geo::kMenuW);
    if (x < screen.left())
        x = snap(screen.left());

    mRect = Rect(x, y, geo::kMenuW, h);
    mOpen = true;
    mArmed = -1;
    mHover = -1;
    mHighlight = 0;
}

void Menu::close()
{
    mOpen = false;
    mArmed = -1;
    mHover = -1;
}

//------------------------------------------------------------------------
Rect Menu::rowRect(int row) const
{
    return Rect(mRect.x, mRect.y + geo::kMenuPadY + static_cast<float>(row) * geo::kMenuRowH,
                mRect.w, geo::kMenuRowH);
}

int Menu::rowAt(float x, float y) const
{
    if (!mOpen || !mRect.contains(x, y))
        return -1;
    for (size_t i = 0; i < mItems.size(); ++i) {
        if (rowRect(static_cast<int>(i)).contains(x, y))
            return static_cast<int>(i);
    }
    return -1;
}

//------------------------------------------------------------------------
void Menu::motion(float x, float y)
{
    if (!mOpen)
        return;
    mHover = rowAt(x, y);
    if (mHover >= 0)
        mHighlight = mHover;
}

//------------------------------------------------------------------------
void Menu::activateRow(int row)
{
    if (row < 0 || row >= static_cast<int>(mItems.size()))
        return;

    const MenuItem &item = mItems[static_cast<size_t>(row)];

    if (item.destructive && mArmed != row) {
        // First press on a destructive row arms it and does nothing else. Any previously
        // armed row is disarmed by the same assignment, so only one can ever be live.
        mArmed = row;
        return;
    }

    mArmed = -1;
    if (activate)
        activate(item);
}

//------------------------------------------------------------------------
bool Menu::click(float x, float y)
{
    if (!mOpen)
        return false;

    if (!mRect.contains(x, y)) {
        // A click anywhere else dismisses the menu, and is swallowed rather than passed on --
        // the first click outside an open menu closes it and does nothing more, which is what
        // every menu does and what stops a mis-aimed dismissal pressing Log in.
        close();
        return true;
    }

    const int row = rowAt(x, y);
    if (row >= 0)
        activateRow(row);
    return true;
}

//------------------------------------------------------------------------
bool Menu::key(Key k)
{
    if (!mOpen)
        return false;

    const int n = static_cast<int>(mItems.size());
    if (n == 0)
        return true;

    switch (k) {
        case Key::Up:
            mHighlight = (mHighlight - 1 + n) % n;
            // Moving off an armed row disarms it: the confirmation belongs to the row, not to
            // the menu, and a Shutdown left armed while the highlight is elsewhere is exactly
            // the accident the confirm step exists to prevent.
            if (mArmed != mHighlight)
                mArmed = -1;
            mHover = -1;
            return true;

        case Key::Down:
            mHighlight = (mHighlight + 1) % n;
            if (mArmed != mHighlight)
                mArmed = -1;
            mHover = -1;
            return true;

        case Key::Home:
            mHighlight = 0;
            mArmed = -1;
            return true;

        case Key::End:
            mHighlight = n - 1;
            mArmed = -1;
            return true;

        case Key::Enter:
            activateRow(mHighlight);
            return true;

        case Key::Escape:
            // Escape backs out one step at a time: first it disarms a confirmation, and only
            // then does it close the menu. Somebody who has just armed Shutdown by accident
            // presses Escape, and the thing they wanted undone is the arming.
            if (mArmed >= 0)
                mArmed = -1;
            else
                close();
            return true;

        default:
            // Everything else is swallowed while the menu is open, so a keystroke meant for
            // the menu cannot land in the password field behind it.
            return true;
    }
}

//------------------------------------------------------------------------
void Menu::draw(Canvas &c) const
{
    if (!mOpen)
        return;

    // The surface. Opaque, because it may be over a photograph.
    c.setColor(pal::kFaceColor);
    c.fillRoundRect(mRect, geo::kMenuRadius);
    c.setColor(pal::kGold, geo::kRuleAlpha);
    c.setPenSize(1.0f);
    c.strokeRoundRect(mRect, geo::kMenuRadius);

    c.setFont(Font::Body);
    c.setFontSize(geo::kMenuTextSize);

    for (size_t i = 0; i < mItems.size(); ++i) {
        const int row = static_cast<int>(i);
        const MenuItem &item = mItems[i];
        const Rect r = rowRect(row);
        const bool armed = (mArmed == row);
        const bool lit = (mHover == row) || (mHighlight == row && mHover < 0);

        if (item.startsGroup && row > 0) {
            const float sy = snap(r.top());
            c.setColor(pal::kGold, geo::kRuleAlpha);
            c.setPenSize(1.0f);
            c.strokeLine(r.left() + geo::kMenuSeparatorInset, sy,
                         r.right() - geo::kMenuSeparatorInset, sy);
        }

        // An armed row is washed in the warn colour so that it does not look like the row
        // that was just clicked. A merely highlighted row gets the well fill and nothing else.
        const Rect fill = Rect(r.x + 3.0f, r.y + 1.0f, r.w - 6.0f, r.h - 2.0f);
        if (armed) {
            c.setColor(pal::kWarnColor, kOnFillAlpha);
            c.fillRoundRect(fill, geo::kFieldRadius);
        } else if (lit) {
            c.setColor(pal::kWellColor);
            c.fillRoundRect(fill, geo::kFieldRadius);
        }

        if (lit || armed) {
            c.setColor(armed ? pal::kWarnColor : inkFor(true, false), kOutlineAlphaHover);
            c.setPenSize(1.0f);
            c.strokeRoundRect(fill, geo::kFieldRadius);
        }

        std::string label = item.label;
        if (armed)
            label = "Confirm: " + item.label;

        c.setColor(armed ? pal::kTextColor
                         : (item.destructive ? pal::kWarnColor : pal::kTextColor));
        const float slot = r.w - 2.0f * geo::kMenuPadX;
        c.drawString(c.clipToWidth(label, slot).c_str(), r.x + geo::kMenuPadX,
                     r.centerY() + geo::kMenuTextSize * geo::kLabelBaselineBias);
    }
}

} // namespace xlogin
