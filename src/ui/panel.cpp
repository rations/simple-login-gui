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

    // Forwarded rather than handled: what a machine can be asked to do is not the layout's
    // business, and the panel would otherwise have to know what a VT is.
    mMenu.activate = [this](const MenuItem &item) {
        if (cb.menuAction)
            cb.menuAction(item);
    };
}

void Panel::openMenu()
{
    mMenu.open(mOptions.rect, mScreen);
    setFocus(Focus::Options);
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
    mMenu.close();
    setFocus(mUser.empty() ? Focus::User : Focus::Pass);
}

void Panel::setFocus(Focus f)
{
    mFocus = f;
    mUser.setFocused(f == Focus::User);
    mPass.setFocused(f == Focus::Pass);
}

// Move round the ring, skipping anything disabled. `delta` is +1 or -1.
//
// The loop is bounded by the number of stops rather than by "until we find an enabled one",
// because while authentication is running only Options is enabled and an unbounded search
// would spin forever on a panel where nothing was.
void Panel::focusNext(int delta)
{
    const Focus ring[] = {Focus::User, Focus::Pass, Focus::Options, Focus::Login};
    const int n = 4;

    int at = 0;
    for (int i = 0; i < n; ++i) {
        if (ring[i] == mFocus)
            at = i;
    }

    for (int step = 0; step < n; ++step) {
        at = (at + delta + n) % n;
        const Focus f = ring[at];
        const bool usable = (f == Focus::User || f == Focus::Pass) ? mEnabled
                            : (f == Focus::Login)                  ? mLogin.enabled
                                                                   : mOptions.enabled;
        if (usable) {
            setFocus(f);
            return;
        }
    }
}

//------------------------------------------------------------------------
void Panel::motion(float x, float y)
{
    if (mMenu.isOpen()) {
        mMenu.motion(x, y);
        // Nothing behind an open menu highlights: a control lighting up under a menu that
        // covers it reads as the menu being transparent to the pointer, which it is not.
        mUser.setHovered(false);
        mPass.setHovered(false);
        mLogin.hovered = false;
        mOptions.hovered = false;
        return;
    }

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

    // An open menu gets every click, including the ones outside it -- that click closes it
    // and is consumed, so a mis-aimed dismissal cannot land on Log in.
    if (mMenu.isOpen()) {
        mMenu.click(x, y);
        return;
    }

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
    if (mUser.rect().contains(x, y) && mUser.enabled()) {
        setFocus(Focus::User);
        mUser.handleClick(x, y);
        return;
    }
    if (mPass.rect().contains(x, y) && mPass.enabled()) {
        setFocus(Focus::Pass);
        mPass.handleClick(x, y);
        return;
    }
}

//------------------------------------------------------------------------
void Panel::key(Key k, const char *utf8, int len)
{
    // The menu is modal while it is open, including when the rest of the panel is disabled.
    // That is the case that matters: if PAM has hung, the menu is the only thing on this
    // screen that still works, and it has to work from the keyboard.
    if (mMenu.isOpen()) {
        mMenu.key(k);
        return;
    }

    if (!mEnabled) {
        // Everything but the fields is off, but the ring still has Options on it -- so a
        // stuck authentication can still be escaped without a pointer.
        if (k == Key::Tab || k == Key::BackTab)
            focusNext(k == Key::Tab ? 1 : -1);
        else if (k == Key::Enter && mFocus == Focus::Options && cb.options)
            cb.options();
        return;
    }

    // The focused field gets first refusal and consumes everything that is editing or text.
    if (mUser.handleKey(k, utf8, len) || mPass.handleKey(k, utf8, len))
        return;

    switch (k) {
        case Key::Tab:
            focusNext(1);
            return;
        case Key::BackTab:
            focusNext(-1);
            return;

        // The arrows move between the two fields only. Inside the ring they would mean
        // "leave the form", which is not what an arrow key means anywhere else.
        case Key::Up:
            if (mFocus == Focus::Pass)
                setFocus(Focus::User);
            return;
        case Key::Down:
            if (mFocus == Focus::User)
                setFocus(Focus::Pass);
            return;

        case Key::Enter:
            if (mFocus == Focus::Options) {
                if (cb.options)
                    cb.options();
                return;
            }
            // Return in the username field moves on rather than submitting, which is what
            // every other login screen does and what muscle memory expects. Return in the
            // password field, or with the button focused, submits.
            if (mFocus == Focus::User && mPass.empty()) {
                setFocus(Focus::Pass);
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
            setFocus(Focus::User);
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

    // A focused button is drawn as though hovered. One treatment for "this is where the
    // next Return goes", whether it got there by pointer or by Tab.
    Button options = mOptions;
    Button login = mLogin;
    options.hovered = options.hovered || mFocus == Focus::Options;
    login.hovered = login.hovered || (mFocus == Focus::Login && login.enabled);
    options.draw(c);
    login.draw(c);

    // Last, and over everything: the menu is the only thing on this screen that is allowed
    // to leave the panel.
    mMenu.draw(c);
}

} // namespace xlogin
