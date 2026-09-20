// X11Window -- the one fullscreen window this program paints, by hand, through Canvas.
//
// Ported from the sibling CPU-Power window, which was itself ported from the rations-amp
// plug-in editor. Four rules carry over unchanged, because they are the ones that bite:
//
//   * DOUBLE BUFFER, ALWAYS. Compose into an ARGB32 image surface, blit once with
//     CAIRO_OPERATOR_SOURCE. A partially drawn frame is never visible.
//   * NEVER PAINT SYNCHRONOUSLY FROM AN EVENT HANDLER. Handlers set a dirty flag; the paint
//     happens once per pass round the loop, so a burst of motion events costs one repaint.
//   * INSTALL THE NON-FATAL X ERROR HANDLER (xerror.h) before creating anything.
//   * DETECT ASYNCHRONOUS XCreateWindow FAILURE by counting X errors across an XSync. X requests
//     do not fail in place, so a window that was never created otherwise shows up much later as
//     an unrelated BadDrawable.
//
// LAYOUT IS IN LOGICAL UNITS. One cairo_scale is applied at compose time and pointer
// coordinates are divided by the scale before they reach the callbacks. No geometry constant
// anywhere has a scale factor baked into it.
//
// WHAT IS DIFFERENT HERE, AND WHY: THERE IS NO WINDOW MANAGER.
//
// This runs on a bare X server before login. Nothing will place the window, nothing will size
// it, nothing will raise it and -- the one that actually bites -- nothing will give it the
// keyboard focus. So:
//
//   * The window is created override_redirect and covers the whole screen. There is no
//     WM_DELETE_WINDOW, no XSetWMProtocols, no XSetClassHint and no size hints: every one of
//     those is a message to a window manager that is not running. There is no _NET_WM_ICON
//     either, for the same reason -- no titlebar, no task list.
//   * We call XSetInputFocus ourselves. Without it the server's focus stays at PointerRoot,
//     which means keystrokes go to whatever window the pointer happens to be over. That is why
//     the GTK version of this program only accepted typing when the pointer was over it.
//   * We grab the keyboard while a password is being typed. The X server is started with -ac,
//     so access control is off and any client that can reach the display could otherwise read
//     the password as it is entered.
//   * We set the pointer cursor, because otherwise the bare X server's default is shown.
//   * Text input goes through XIM/XIC and Xutf8LookupString rather than XLookupString. A login
//     screen that cannot type an accented character cannot log in a user whose password has
//     one. Every event is offered to XFilterEvent first, which is how the input method gets to
//     consume the keystrokes that make up a dead-key or Compose sequence.
//
// The loop also takes extra file descriptors, which is what lets the session's SIGCHLD
// self-pipe be waited on in the same select() as the X connection -- so the window keeps
// repainting while a session runs, with no second loop and no blocking wait on a child.

#pragma once

#include "gfx/canvas.h"
#include "gfx/fontstack.h"

#include <X11/Xlib.h>

#include <functional>
#include <string>
#include <vector>

namespace xlogin
{

class X11Window
{
public:
    X11Window() = default;
    ~X11Window();

    X11Window(const X11Window &) = delete;
    X11Window &operator=(const X11Window &) = delete;

    // All coordinates handed to these are LOGICAL units, already divided by the scale.
    struct Callbacks {
        std::function<void(Canvas &)> draw;
        // pressed = true on ButtonPress, false on ButtonRelease. Button 1 only.
        std::function<void(float x, float y, bool pressed)> button;
        std::function<void(float x, float y)> motion;
        // One key press. `sym` is the keysym for the navigation and editing keys; `text` is the
        // UTF-8 the input method produced, which is empty for a key that produces no text and
        // can be more than one byte -- or, after a dead-key sequence, more than one character.
        // `state` is the raw X modifier mask. Return value is currently advisory; unlike the
        // sibling window, an unhandled Escape does NOT close this one. There is nothing behind
        // it to close to.
        std::function<bool(KeySym sym, const char *text, int len, unsigned state)> key;
        // Called every tickMs, whether or not anything is dirty.
        std::function<void()> tick;
    };

    // Opens the display, covers the screen, loads the fonts. Returns false having already
    // warned. `scale` maps logical units to pixels; `tickMs` is the idle tick period.
    bool open(const std::string &title, float scale, int tickMs);

    void run(const Callbacks &cb);
    void stop()
    {
        mRunning = false;
    }
    // Ask for a repaint on the next pass. Cheap and idempotent -- call it from any handler.
    void invalidate()
    {
        mDirty = true;
    }

    // Repaint NOW, from inside a handler, and only for the case that needs it: a handler about
    // to block for a noticeable time. PAM authentication against a slow module, and the moment
    // between "Log in" being clicked and the session appearing, both leave a stale frame up
    // otherwise -- with the status line still saying whatever it said before the click -- which
    // reads as a hang. Every other repaint goes through invalidate() and the loop's one paint
    // per pass, which is the rule this is the deliberate exception to.
    void paintNow()
    {
        if (mActive) {
            mDirty = false;
            paint(*mActive);
        }
    }

    // Wait on this fd alongside the X connection, calling `onReadable` when it has data. Used
    // for the session child's SIGCHLD self-pipe. The fd is not owned and must outlive the loop.
    void addFd(int fd, std::function<void()> onReadable);
    void removeFd(int fd);

    //--- focus, grab and cursor ----------------------------------------
    // Take the keyboard. Retries briefly: immediately after the X server starts, or just after
    // a session's clients have been killed, another client can still hold a grab for a moment.
    // Returns false if it could not be taken, which is a warning and not a fatal error -- a
    // login screen that will not accept typing is worse than one typed without a grab.
    bool grabKeyboard();
    void ungrabKeyboard();
    bool keyboardGrabbed() const
    {
        return mGrabbed;
    }
    // Raise, map and take the input focus. Safe to call repeatedly; used again after a session
    // exits and the login screen comes back.
    void takeFocus();

    //--- geometry ------------------------------------------------------
    // The whole screen, in logical units.
    Rect bounds() const
    {
        return Rect(0, 0, mLogicalW, mLogicalH);
    }
    // The primary output's rectangle in logical units, for centring the panel on a multi-head
    // machine. Falls back to bounds() when Xrandr reports nothing usable, which is also what a
    // single-head machine gets.
    Rect primaryBounds() const
    {
        return mPrimary;
    }

    bool fontsAreBundled() const
    {
        return mFontsLoaded;
    }

private:
    void paint(const Callbacks &cb);
    void close();
    void resolvePrimary();
    void openInputMethod();
    void closeInputMethod();

    struct ExtraFd {
        int fd = -1;
        std::function<void()> onReadable;
    };

    ::Display *mDpy = nullptr;
    ::Window mWin = 0;
    ::Cursor mCursor = 0;
    cairo_surface_t *mTarget = nullptr;

    XIM mXim = nullptr;
    XIC mXic = nullptr;

    FontStack mFonts;
    bool mFontsLoaded = false;

    int mPixelW = 0, mPixelH = 0;
    float mLogicalW = 0, mLogicalH = 0, mScale = 1.0f;
    Rect mPrimary;
    int mTickMs = 250;
    // The callbacks run() was given, so paintNow() can compose the same frame the loop would.
    // Null outside run().
    const Callbacks *mActive = nullptr;

    std::vector<ExtraFd> mExtraFds;

    bool mRunning = false;
    bool mDirty = true;
    bool mGrabbed = false;
};

} // namespace xlogin
