// See panel.h.

#include "panel.h"

#include "../geometry.h"
#include "../gfx/ink.h"
#include "../gfx/palette.h"

#include <cmath>

namespace xlogin
{

namespace
{

// Round a panel origin to a whole logical unit. A 1px hairline drawn at a half-unit origin
// straddles two rows of pixels and comes out grey and two pixels wide; Canvas pins the
// half-pixel offset that fixes that, and it only works from an integral origin.
inline float snap(float v)
{
    return std::floor(v + 0.5f);
}

} // namespace

//------------------------------------------------------------------------
void Panel::layout(const Rect &screen, const Rect &primary)
{
    mScreen = screen;

    // Centre on the primary output, then clamp into the screen. The clamp is not decoration: a
    // 1024x600 netbook has room for the panel but not for it to be centred on an output whose
    // reported origin is off to one side, and a panel drawn off the edge of a login screen is
    // a machine nobody can log into.
    const Rect on = (primary.w >= geo::kPanelW && primary.h >= geo::kPanelH) ? primary : screen;

    float px = snap(on.centerX() - geo::kPanelW * 0.5f);
    float py = snap(on.centerY() - geo::kPanelH * 0.5f);
    if (px + geo::kPanelW > screen.right())
        px = snap(screen.right() - geo::kPanelW);
    if (py + geo::kPanelH > screen.bottom())
        py = snap(screen.bottom() - geo::kPanelH);
    if (px < screen.left())
        px = snap(screen.left());
    if (py < screen.top())
        py = snap(screen.top());

    mPanel = Rect(px, py, geo::kPanelW, geo::kPanelH);

    mUser.configure(Rect(px + geo::kFieldX, py + geo::kUserFieldY, geo::kFieldW, geo::kFieldH),
                    "username", false);
    mPass.configure(Rect(px + geo::kFieldX, py + geo::kPassFieldY, geo::kFieldW, geo::kFieldH),
                    "password", true);

    mLogin.rect = Rect(px + geo::kLoginX, py + geo::kButtonY, geo::kLoginW, geo::kButtonH);
    mLogin.label = "Log in";
    mLogin.accent = true;

    mOptions.rect = Rect(px + geo::kOptionsX, py + geo::kButtonY, geo::kOptionsW, geo::kButtonH);
    mOptions.label = "Options";
}

//------------------------------------------------------------------------
void Panel::setStatus(const std::string &text, bool isError)
{
    mStatus = text;
    mStatusIsError = isError;
}

void Panel::clearStatus()
{
    mStatus.clear();
    mStatusIsError = false;
}

void Panel::setEnabled(bool e)
{
    mEnabled = e;
    mUser.setEnabled(e);
    mPass.setEnabled(e);
    mLogin.enabled = e;
    // mOptions stays enabled. If authentication has hung, the Options menu is the only way off
    // this screen, so it is the one control that is never disabled.
}

void Panel::reset()
{
    mPass.clear();
    clearStatus();
    setEnabled(true);
    if (mUser.empty())
        focusUser();
    else
        focusPass();
}

void Panel::focusUser()
{
    mUser.setFocused(true);
    mPass.setFocused(false);
}

void Panel::focusPass()
{
    mUser.setFocused(false);
    mPass.setFocused(true);
}

//------------------------------------------------------------------------
void Panel::motion(float x, float y)
{
    mUser.setHovered(mUser.rect().contains(x, y));
    mPass.setHovered(mPass.rect().contains(x, y));
    mLogin.hovered = mLogin.enabled && mLogin.rect.contains(x, y);
    mOptions.hovered = mOptions.enabled && mOptions.rect.contains(x, y);
}

void Panel::click(float x, float y, bool pressed)
{
    // Act on release, not press. A button that fires under the finger cannot be escaped by
    // moving off it before letting go, and one of these buttons will eventually open a menu
    // whose entries power the machine off.
    if (pressed)
        return;

    if (mOptions.hit(x, y)) {
        if (cb.options)
            cb.options();
        return;
    }
    if (mLogin.hit(x, y)) {
        if (cb.submit)
            cb.submit();
        return;
    }
    if (mUser.handleClick(x, y)) {
        mPass.setFocused(false);
        return;
    }
    if (mPass.handleClick(x, y)) {
        mUser.setFocused(false);
        return;
    }
}

//------------------------------------------------------------------------
void Panel::key(Key k, const char *utf8, int len)
{
    if (!mEnabled)
        return;

    // The focused field gets first refusal, and consumes everything that is editing or text.
    if (mUser.handleKey(k, utf8, len) || mPass.handleKey(k, utf8, len))
        return;

    switch (k) {
        // With exactly two fields, forward and backward are the same move, so Tab, Shift+Tab
        // and the arrows all toggle. Splitting them would be two code paths that can only ever
        // agree.
        case Key::Tab:
        case Key::BackTab:
        case Key::Up:
        case Key::Down:
            if (userFocused())
                focusPass();
            else
                focusUser();
            return;

        case Key::Enter:
            // Return in the username field moves on rather than submitting, which is what
            // every other login screen does and what muscle memory expects. Return in the
            // password field, or in either field once both are filled, submits.
            if (userFocused() && mPass.empty()) {
                focusPass();
                return;
            }
            if (cb.submit)
                cb.submit();
            return;

        case Key::Escape:
            // Clears the fields rather than closing anything: there is nothing behind this
            // window to close to, and somebody who has mistyped wants an empty form.
            mPass.clear();
            mUser.clear();
            clearStatus();
            focusUser();
            return;

        default:
            return;
    }
}

//------------------------------------------------------------------------
void Panel::draw(Canvas &c) const
{
    // The ground, always, even when there is a background image: the image may not cover the
    // screen in fit or centre mode, and an uncovered screen must not be whatever the X server
    // left in the framebuffer.
    c.setColor(pal::kBgColor);
    c.fillRect(mScreen);
    drawBackground(c, mBackground, mScreen, mBgMode);

    // The panel face, with the gold hairline round it. Over a photograph the face needs to be
    // opaque, or the form becomes unreadable on a light image.
    c.setColor(pal::kFaceColor);
    c.fillRoundRect(mPanel, geo::kPanelRadius);
    c.setColor(pal::kGold, geo::kRuleAlpha);
    c.setPenSize(1.0f);
    c.strokeRoundRect(mPanel, geo::kPanelRadius);

    const float px = mPanel.x;
    const float py = mPanel.y;

    // Title, in the display face.
    c.setFont(Font::Title);
    c.setFontSize(geo::kTitleSize);
    c.setColor(pal::kTextColor);
    c.drawString("LOGIN", px + geo::kMargin, py + geo::kTitleBaselineY);

    c.setColor(pal::kGold, geo::kRuleAlpha);
    c.setPenSize(1.0f);
    c.strokeLine(px + geo::kMargin, py + geo::kRuleY, px + geo::kPanelW - geo::kMargin,
                 py + geo::kRuleY);

    // Hostname. Not decoration: on a machine with more than one of these it is the only thing
    // on the screen that says which one you are about to log into.
    c.setFont(Font::Body);
    c.setFontSize(geo::kHostSize);
    c.setColor(pal::kDimColor);
    c.drawString(c.clipToWidth(mHost, geo::kPanelW - 2.0f * geo::kMargin).c_str(),
                 px + geo::kMargin, py + geo::kHostBaselineY);

    // Field labels.
    c.setFontSize(geo::kLabelSize);
    c.setColor(pal::kDimColor);
    c.drawString("Username", px + geo::kFieldX, py + geo::kUserLabelBaselineY);
    c.drawString("Password", px + geo::kFieldX, py + geo::kPassLabelBaselineY);

    mUser.draw(c);
    mPass.draw(c);

    // The status line. One line, clipped: it carries pam_strerror's text, which is not written
    // to a width, and a message that wraps out of the panel is worse than one with an ellipsis.
    if (!mStatus.empty()) {
        c.setFont(Font::Body);
        c.setFontSize(geo::kStatusSize);
        c.setColor(mStatusIsError ? pal::kErrorColor : pal::kDimColor);
        c.drawString(c.clipToWidth(mStatus, geo::kStatusW).c_str(), px + geo::kStatusX,
                     py + geo::kStatusBaselineY);
    }

    mOptions.draw(c);
    mLogin.draw(c);
}

} // namespace xlogin
