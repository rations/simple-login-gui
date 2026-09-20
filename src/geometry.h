// The whole window's layout, in logical units, in one place.
//
// EVERY coordinate the login screen draws at is here. Nothing downstream computes a position
// from a magic number, and nothing here has a scale factor baked into it -- exactly one
// cairo_scale is applied when the frame is composed (platform/x11window.cpp), and pointer
// coordinates are divided by that same scale before they reach a hit test. So a constant in
// this file means the same thing whether the screen is 1366x768 or 4K.
//
// THE PANEL IS A FIXED 420x300 BOX CENTRED ON THE PRIMARY OUTPUT. It does not stretch with the
// screen: a login form stretched across a 34" monitor puts the username field a foot away from
// the password field. What scales is the unit, not the layout.
//
// THE static_asserts AT THE BOTTOM ARE THE POINT OF THE FILE. Every clearance between two parts
// is asserted against conservative nominal font metrics, so a constant edited to a wrong value
// fails the build rather than clipping a label on somebody else's machine. They are a cheap
// guard and not the real check: the real check is tools/uirender, which measures the actual
// strings against the actual faces. Both exist because the string most likely to be clipped
// here is the status line, and the status line is what tells a user why they cannot log in.

#pragma once

namespace xlogin
{
namespace geo
{

//--- nominal font metrics, for the compile-time asserts only --------------------------------
//
// Not measurements of any particular string: the font's own box, which is the conservative
// bound. Roboto's ascender is 0.927em and its descender 0.244em (its OS/2 table); these are
// rounded outward so an assert can never pass on a rounding error. Real per-string ink extents
// come from Canvas::stringAscent/stringDescent and are what uirender checks.
constexpr float nominalAscent(float size)
{
    return size * 0.95f;
}
constexpr float nominalDescent(float size)
{
    return size * 0.26f;
}

//--- the panel ------------------------------------------------------------------------------
constexpr float kPanelW = 420.0f;
constexpr float kPanelH = 300.0f;
constexpr float kMargin = 16.0f;
constexpr float kPanelRadius = 6.0f;

// The gold hairline is drawn at this alpha, the same understated weight the sibling projects
// use for the line under a title -- at full strength it reads as a border rather than a rule.
constexpr int kRuleAlpha = 90;

//--- title and host -------------------------------------------------------------------------
constexpr float kTitleSize = 15.0f; // Michroma
constexpr float kTitleBaselineY = 36.0f;
constexpr float kRuleY = 50.0f;

constexpr float kHostSize = 10.0f; // Roboto
constexpr float kHostBaselineY = 68.0f;

//--- the two fields ---------------------------------------------------------------------------
constexpr float kLabelSize = 10.0f;
constexpr float kFieldTextSize = 12.0f;
constexpr float kFieldH = 30.0f;
constexpr float kFieldX = kMargin;
constexpr float kFieldW = kPanelW - 2.0f * kMargin;
constexpr float kFieldRadius = 3.0f;
// Clearance between a field's edge and its text, on both sides. The right-hand one matters:
// it is what stops the caret being drawn on top of the outline at the end of a full field.
constexpr float kFieldPadX = 10.0f;

constexpr float kUserLabelBaselineY = 92.0f;
constexpr float kUserFieldY = 98.0f;
constexpr float kPassLabelBaselineY = 148.0f;
constexpr float kPassFieldY = 154.0f;

// A masked password is drawn as dots, NOT as a bullet glyph. Two reasons, and the second is the
// one that matters: a glyph depends on the body face actually having U+2022, and -- the point --
// drawing dots means the plaintext is never handed to cairo to be measured or shaped at all.
// The caret is placed by counting characters, so nothing downstream of the field ever sees the
// password. Pitch and radius are set so the dots read as a password at 12px text.
constexpr float kDotRadius = 2.2f;
constexpr float kDotPitch = 9.0f;

// A 1px line, because anything thicker sits between two characters rather than at a position.
constexpr float kCaretW = 1.0f;

//--- status line --------------------------------------------------------------------------
constexpr float kStatusSize = 11.0f;
constexpr float kStatusBaselineY = 206.0f;
constexpr float kStatusX = kMargin;
constexpr float kStatusW = kPanelW - 2.0f * kMargin;

//--- the button row -------------------------------------------------------------------------
constexpr float kButtonH = 32.0f;
constexpr float kButtonY = kPanelH - kMargin - kButtonH;
constexpr float kButtonRadius = 3.0f;
constexpr float kButtonTextSize = 12.0f;

constexpr float kLoginW = 130.0f;
constexpr float kLoginX = kPanelW - kMargin - kLoginW;

constexpr float kOptionsW = 110.0f;
constexpr float kOptionsX = kMargin;

// A label's baseline inside a box that is meant to look vertically centred. 0.36 of the font
// size below the geometric centre, which is the sibling projects' constant -- optical centring,
// because a line of text has more ink above its centre than below it.
constexpr float kLabelBaselineBias = 0.36f;

//--- the Options menu -----------------------------------------------------------------------
//
// Drawn in SCREEN coordinates, not panel coordinates: it opens above the Options button and is
// allowed to extend past the panel's edge, because the alternative is either a cramped menu or
// a taller panel that is mostly empty whenever the menu is shut.
constexpr float kMenuW = 230.0f;
constexpr float kMenuRowH = 28.0f;
constexpr float kMenuPadY = 6.0f;
constexpr float kMenuPadX = 12.0f;
constexpr float kMenuRadius = 4.0f;
constexpr float kMenuTextSize = 12.0f;
// Between the top of the Options button and the bottom of the menu. Enough that the menu
// reads as a separate surface and not as a taller button.
constexpr float kMenuGap = 8.0f;
// The separator drawn above a trailing group, at the rule alpha.
constexpr float kMenuSeparatorInset = 8.0f;

constexpr float menuHeight(int rows)
{
    return 2.0f * kMenuPadY + static_cast<float>(rows) * kMenuRowH;
}

static_assert(kMenuPadX > kFieldPadX * 0.5f, "menu labels would sit too close to the edge");
static_assert(kMenuW > kOptionsW, "the menu should be wider than the button that opens it");
// A menu row must be able to hold its label without the descenders touching the row below.
static_assert(kMenuRowH > nominalAscent(kMenuTextSize) + nominalDescent(kMenuTextSize),
              "a menu row is shorter than the text in it");

//--- clearances, asserted -------------------------------------------------------------------

// The title's descender must clear the rule, and the rule must clear the host line's ascender.
static_assert(kTitleBaselineY + nominalDescent(kTitleSize) < kRuleY,
              "the title's descenders would cross the gold rule");
static_assert(kRuleY < kHostBaselineY - nominalAscent(kHostSize),
              "the host line's ascenders would cross the gold rule");
static_assert(kTitleBaselineY - nominalAscent(kTitleSize) >= kMargin,
              "the title's ascenders would leave the panel's top margin");

// The host line must clear the first label, the first label its field, and so on down.
static_assert(kHostBaselineY + nominalDescent(kHostSize) <
                  kUserLabelBaselineY - nominalAscent(kLabelSize),
              "the host line and the username label would collide");
static_assert(kUserLabelBaselineY + nominalDescent(kLabelSize) < kUserFieldY,
              "the username label's descenders would cross into its field");
static_assert(kUserFieldY + kFieldH < kPassLabelBaselineY - nominalAscent(kLabelSize),
              "the username field and the password label would collide");
static_assert(kPassLabelBaselineY + nominalDescent(kLabelSize) < kPassFieldY,
              "the password label's descenders would cross into its field");

// Field text must sit inside its own well, both ways.
constexpr float fieldTextBaseline(float fieldY)
{
    return fieldY + kFieldH * 0.5f + kFieldTextSize * kLabelBaselineBias;
}
static_assert(fieldTextBaseline(kUserFieldY) - nominalAscent(kFieldTextSize) > kUserFieldY,
              "field text would be clipped by the top of its well");
static_assert(fieldTextBaseline(kUserFieldY) + nominalDescent(kFieldTextSize) <
                  kUserFieldY + kFieldH,
              "field text would be clipped by the bottom of its well");

// The status line must clear the password field above it and the button row below it. This is
// the tightest pair in the panel and the one most likely to be broken by an edit.
static_assert(kPassFieldY + kFieldH < kStatusBaselineY - nominalAscent(kStatusSize),
              "the password field and the status line would collide");
static_assert(kStatusBaselineY + nominalDescent(kStatusSize) < kButtonY,
              "the status line's descenders would cross into the button row");

// The buttons must not overlap each other, and the row must sit inside the bottom margin.
static_assert(kOptionsX + kOptionsW < kLoginX, "the Options and Log in buttons would overlap");
static_assert(kLoginX + kLoginW + kMargin <= kPanelW, "the Log in button overruns the panel");
static_assert(kButtonY + kButtonH + kMargin <= kPanelH, "the button row overruns the panel");
static_assert(fieldTextBaseline(kButtonY + (kButtonH - kFieldH) * 0.5f) <
                  kButtonY + kButtonH - nominalDescent(kButtonTextSize),
              "a button label's descenders would be clipped by the button");

// The dots of a full-width masked password must have room for the caret after the last one.
static_assert(kFieldPadX > kCaretW, "the caret would be drawn on top of the field outline");
static_assert(kDotPitch > 2.0f * kDotRadius, "password dots would touch each other");

} // namespace geo
} // namespace xlogin
