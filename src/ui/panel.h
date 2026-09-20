// Panel -- everything on the screen, drawn and hit-tested.
//
// Immediate mode: there is no widget tree and no retained layout. layout() positions the
// controls from geometry.h once, draw() paints them, and the hit tests read THE SAME RECTS the
// controls were drawn with -- which is the whole reason immediate mode is acceptable here, and
// it stops being true the moment somebody inlines a rectangle instead of asking the control
// for its own.
//
// THIS FILE LINKS CAIRO AND NOT X11, deliberately. It is the entire visible surface of the
// program, so keeping it free of Xlib is what lets tools/uirender render and measure the real
// login screen -- the real fonts, the real strings, the real clearances -- with no X server
// running. Keystrokes arrive as gfx/keys.h values that the window has already translated.
//
// The panel is a fixed 420x300 box centred on the primary output. It does not know what a
// session is, what PAM is, or that a machine can be shut down: it reports that a button was
// pressed and its owner decides what that means.

#pragma once

#include "../gfx/canvas.h"
#include "../gfx/image.h"
#include "../gfx/keys.h"
#include "../gfx/menu.h"
#include "../gfx/textfield.h"
#include "../gfx/widgets.h"

#include <functional>
#include <string>

namespace xlogin
{

class Panel
{
public:
    struct Callbacks {
        // The user asked to log in -- by clicking Log in, or by pressing Return in either
        // field. The owner reads username() and password() and decides.
        std::function<void()> submit;
        // The Options button was pressed. The owner fills the menu and calls openMenu(),
        // rather than the panel deciding what is on it -- what a machine can be asked to do
        // is not a layout question.
        std::function<void()> options;
        // A menu row was activated, past its confirmation step if it had one.
        std::function<void(const MenuItem &)> menuAction;
    };
    Callbacks cb;

    // Position everything. `screen` is the whole display and `primary` the output to centre on
    // (they are the same thing on a single-head machine). Safe to call again.
    void layout(const Rect &screen, const Rect &primary);

    void draw(Canvas &c) const;

    //--- input ---------------------------------------------------------
    // Pointer position in logical units. (-1, -1) means the pointer left the window, which
    // clears every hover state -- otherwise a control stays lit after the pointer has gone.
    void motion(float x, float y);
    void click(float x, float y, bool pressed);
    void key(Key k, const char *utf8, int len);

    //--- state ---------------------------------------------------------
    void setStatus(const std::string &text, bool isError);
    void clearStatus();
    // Disabled while authenticating and while a session is running: the fields stop accepting
    // input and Log in stops responding. Options stays live, deliberately -- if PAM has hung,
    // the way out of this screen must still work.
    void setEnabled(bool e);
    bool enabled() const
    {
        return mEnabled;
    }
    void setHostname(const std::string &h)
    {
        mHost = h;
    }
    // Not owned, and may be null: a null background is the flat ground, which is what a failed
    // image load degrades to.
    void setBackground(cairo_surface_t *image, BgMode mode)
    {
        mBackground = image;
        mBgMode = mode;
    }

    // Back to the state the login screen starts in: password erased, status cleared, controls
    // live, caret in whichever field is empty. Called at startup and again after a session
    // ends. The username is deliberately KEPT -- somebody whose password was rejected should
    // not have to type their name again.
    void reset();

    //--- the Options menu ----------------------------------------------
    void setMenuItems(std::vector<MenuItem> items)
    {
        mMenu.setItems(std::move(items));
    }
    // Opens above the Options button. Safe to call when it is already open.
    void openMenu();
    void closeMenu()
    {
        mMenu.close();
    }
    bool menuIsOpen() const
    {
        return mMenu.isOpen();
    }

    TextField &username()
    {
        return mUser;
    }
    TextField &password()
    {
        return mPass;
    }
    const Rect &panelRect() const
    {
        return mPanel;
    }
    const Button &optionsButton() const
    {
        return mOptions;
    }

private:
    // The keyboard focus ring. The BUTTONS are in it, not just the fields, and that is the
    // point: the Options menu is this screen's escape route, and an escape route reachable
    // only with a pointer is no use to somebody whose pointer is the reason they need it.
    enum class Focus { User, Pass, Options, Login };

    void setFocus(Focus f);
    void focusNext(int delta);

    Focus mFocus = Focus::User;

    // What the last ButtonPress landed on, so that a release only acts if it comes up over
    // the same thing. Without this, pressing anywhere and releasing over a control activates
    // it -- which means there is no way to change your mind, and the usual way out of a
    // mis-aimed click (move off the control before letting go) does nothing.
    // Nothing, not None: X11/X.h has `#define None 0L` (cites: X11/X.h:141), and src/main.cpp
    // includes both this header and Xlib.h, so `Target::None` expands to `Target::0L` there.
    // Second time this project has hit it, in a header that does not itself include any X
    // header -- which is the part that makes it easy to hit. See also Key::Plain.
    enum class Target { Nothing, User, Pass, Options, Login, MenuRow, MenuOutside };
    Target targetAt(float x, float y, int &row) const;

    Target mPressTarget = Target::Nothing;
    int mPressRow = -1;

    Rect mScreen;
    Rect mPanel;

    TextField mUser;
    TextField mPass;
    Button mLogin;
    Button mOptions;

    std::string mHost;
    std::string mStatus;
    bool mStatusIsError = false;
    bool mEnabled = true;

    Menu mMenu;

    cairo_surface_t *mBackground = nullptr;
    BgMode mBgMode = BgMode::Fill;
};

} // namespace xlogin
