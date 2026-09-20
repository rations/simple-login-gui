// The Options menu: the way off this screen.
//
// This is the feature the whole rewrite was asked for. Before it, a login that failed -- or a
// session that would not start -- left Alt+F2 as the only way out, and somebody who did not
// know that had a machine that appeared to be broken. So: Shutdown, Restart, Console, and the
// background picker.
//
// IT IS A MENU AND NOT A ROW OF RADIO BUTTONS, and that reversal is the point. Radio buttons
// choose a STATE; these are ACTIONS, and an action that fires the instant a widget takes focus
// is one misclick away from powering the machine off while somebody is typing their password.
// A single button opening a popup does the same job, has room for the background list that a
// row of buttons did not, and leaves space for the confirm step:
//
// SHUTDOWN AND RESTART NEED A SECOND CLICK. The row changes in place to "Confirm: Shutdown"
// and only the second click on it does anything. Anything else -- another row, a click
// outside, Escape -- cancels it. Note what this is NOT protecting against: somebody at the
// keyboard who wants the machine off can hold the power button in, and this menu is
// deliberately available to them. It protects against the stray click, which is the thing that
// actually happens.
//
// Keyboard-driven as well as pointer-driven, because the mouse is the part of a machine most
// likely to be the reason somebody is at a login screen wanting a console.
//
// No X11 here: the menu lives in src/gfx/ with everything else that draws, so the headless
// audit renders it too.

#pragma once

#include "canvas.h"
#include "keys.h"

#include <functional>
#include <string>
#include <vector>

namespace xlogin
{

enum class MenuAction {
    Nothing,
    Shutdown,
    Restart,
    Console,
    ShowBackgrounds, // switch this menu to the background list
    BackToMain,
    SetBackground, // `value` names the file; empty means none
    Dismiss,
};

struct MenuItem {
    std::string label;
    MenuAction action = MenuAction::Nothing;
    // For SetBackground: the filename, or empty for "(none)".
    std::string value;
    // Needs a second, confirming click. Drawn in the warn colour once armed.
    bool destructive = false;
    // Draws a separator line above this row.
    bool startsGroup = false;
};

class Menu
{
public:
    // Fired when a row is activated -- after the confirm step for a destructive one, so the
    // owner never has to know the confirm exists.
    std::function<void(const MenuItem &)> activate;

    // Replace the contents. Closes any armed confirmation, because the row it referred to may
    // no longer be there.
    void setItems(std::vector<MenuItem> items);

    // Open above `anchor` (the Options button, in screen coordinates), clamped into `screen`.
    void open(const Rect &anchor, const Rect &screen);
    void close();
    bool isOpen() const
    {
        return mOpen;
    }

    void draw(Canvas &c) const;

    void motion(float x, float y);
    // Returns true if the click belonged to the menu -- including a click outside it, which
    // closes it and is therefore still the menu's business rather than the panel's.
    bool click(float x, float y);
    // Returns true if the key was consumed.
    bool key(Key k);

    const Rect &rect() const
    {
        return mRect;
    }

private:
    void activateRow(int row);
    int rowAt(float x, float y) const;
    Rect rowRect(int row) const;

    std::vector<MenuItem> mItems;
    Rect mRect;
    bool mOpen = false;
    int mHover = -1;
    // The row whose confirmation is armed, or -1. At most one at a time.
    int mArmed = -1;
    // The keyboard's position in the menu, which is not the same thing as the pointer's.
    int mHighlight = 0;
};

} // namespace xlogin
