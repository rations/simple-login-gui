// See x11window.h.

#include "x11window.h"

#include "respath.h"
#include "xerror.h"

#include <cairo/cairo-xlib.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include <sys/select.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace xlogin
{

namespace
{

// How long to keep trying for the keyboard. Immediately after the X server starts, and again
// just after a session's clients have been killed on logout, another client can still hold a
// grab for a moment. 20 attempts at 50 ms is one second, which is far longer than either case
// needs and still short enough that a genuine failure is reported promptly.
constexpr int kGrabAttempts = 20;
constexpr long kGrabRetryNs = 50L * 1000L * 1000L;

void sleepNs(long ns)
{
    struct timespec ts;
    ts.tv_sec = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        // Restart with the remainder nanosleep wrote back.
    }
}

} // namespace

//------------------------------------------------------------------------
X11Window::~X11Window()
{
    close();
}

//------------------------------------------------------------------------
// The scale, when the caller does not pick one.
//
// A fixed 420x300 logical panel drawn at scale 1 is a reasonable dialog on a 1366x768 laptop
// and a postage stamp on a 4K monitor, and this program cannot ask: it draws before there is a
// user to have a preference. So the unit is tied to the screen height, which is the dimension
// a panel's readability actually follows -- 768 is the reference, and the range is clamped so
// that neither a very short screen shrinks the text below legibility nor a very tall one fills
// the screen with a login box.
//
// Quantised to quarter steps rather than left continuous, so that two machines of similar
// height render identically and a screenshot from one is worth comparing with the other.
float X11Window::autoScale(int pixelH)
{
    if (pixelH <= 0)
        return 1.0f;

    float s = static_cast<float>(pixelH) / 768.0f;
    if (s < 1.0f)
        s = 1.0f;
    if (s > 3.0f)
        s = 3.0f;
    return static_cast<float>(static_cast<int>(s * 4.0f + 0.5f)) / 4.0f;
}

//------------------------------------------------------------------------
bool X11Window::open(const std::string &title, float scale, int tickMs)
{
    mScale = scale > 0.0f ? scale : 1.0f;
    mAutoScale = scale <= 0.0f;
    mTickMs = tickMs > 0 ? tickMs : 250;

    mDpy = XOpenDisplay(nullptr);
    if (!mDpy) {
        fprintf(stderr, "xlogin: cannot open the X display ($DISPLAY)\n");
        return false;
    }

    // BEFORE any window exists: Xlib's default handler calls exit(), and the failure this
    // function goes on to detect would otherwise be detected by dying.
    registerDisplay(mDpy);

    const int screen = DefaultScreen(mDpy);
    mRoot = RootWindow(mDpy, screen);
    mPixelW = DisplayWidth(mDpy, screen);
    mPixelH = DisplayHeight(mDpy, screen);
    // The scale and the logical extent are NOT computed here. They come from applyGeometry()
    // below, once the window exists and Xrandr can be asked which output the panel is on --
    // which is also the path a later screen change takes, so there is one implementation of
    // this arithmetic and not two that can drift.

    const unsigned long before = errorCount();

    // override_redirect, covering the screen. There is no window manager to negotiate with, so
    // this is not a hint: it is the only thing that puts the window where it needs to be.
    //
    // The visual is the root's own, so the colormap may be left at CopyFromParent -- the
    // BadMatch trap that makes an explicit CWColormap mandatory applies when reparenting into a
    // foreign parent whose visual may differ, which is the embedded plug-in case this code came
    // from and is not this one.
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(mDpy, screen);
    attrs.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | KeyPressMask | LeaveWindowMask | FocusChangeMask;

    mWin = XCreateWindow(mDpy, RootWindow(mDpy, screen), 0, 0, static_cast<unsigned>(mPixelW),
                         static_cast<unsigned>(mPixelH), 0, CopyFromParent, InputOutput,
                         CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

    XStoreName(mDpy, mWin, title.c_str());

    // The bare server's default cursor is the X-shaped one. Anything else says the screen is a
    // program rather than a server that has not finished starting.
    mCursor = XCreateFontCursor(mDpy, XC_left_ptr);
    if (mCursor)
        XDefineCursor(mDpy, mWin, mCursor);

    XMapRaised(mDpy, mWin);

    // THE ROUND TRIP. XCreateWindow is asynchronous: if it was rejected, nothing above has
    // failed yet and the first symptom would be an unrelated error against a window id that
    // never existed. Sample, sync, sample.
    XSync(mDpy, False);
    if (errorCount() != before) {
        fprintf(stderr, "xlogin: the X server rejected the window (see the error above)\n");
        close();
        return false;
    }

    mTarget = cairo_xlib_surface_create(mDpy, mWin, DefaultVisual(mDpy, screen), mPixelW, mPixelH);
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xlogin: could not create the drawing surface\n");
        close();
        return false;
    }

    // Ask to be told when the screen changes size. This is the login screen's most important
    // subscription after the keyboard: the X server outlives every session, and a session is
    // entitled to set its own video mode and leave it set.
    int rrError = 0;
    mRandr = XRRQueryExtension(mDpy, &mRREventBase, &rrError) == True; // cites: Xrandr.h:160
    if (mRandr) {
        // cites: Xrandr.h:220, randr.h:117
        XRRSelectInput(mDpy, mRoot, RRScreenChangeNotifyMask);
    } else {
        fprintf(stderr, "xlogin: no RandR extension; a screen resize will go unnoticed\n");
    }

    applyGeometry();
    openInputMethod();
    takeFocus();

    mFontsLoaded = mFonts.load(resourceDir());
    if (!mFontsLoaded)
        fprintf(stderr, "xlogin: drawing with a system font; legends may not fit their slots\n");

    return true;
}

//------------------------------------------------------------------------
// Which part of the screen to centre the panel on.
//
// DisplayWidth/DisplayHeight give the bounding box of every output together, so on a dual-head
// machine centring on it puts the panel across the bezel. Xrandr's primary output is the one
// the user calls "the main screen", which is where a login panel belongs.
//
// Every failure here falls back to the whole screen, which is also exactly what a single-head
// machine with no Xrandr configuration reports.
void X11Window::resolvePrimary()
{
    mPrimaryPx = Rect(0, 0, static_cast<float>(mPixelW), static_cast<float>(mPixelH));

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(mDpy, mWin);
    if (!res)
        return;

    const RROutput primary = XRRGetOutputPrimary(mDpy, mWin);
    if (primary != None) {
        if (XRROutputInfo *oi = XRRGetOutputInfo(mDpy, res, primary)) {
            if (oi->crtc != None) {
                if (XRRCrtcInfo *ci = XRRGetCrtcInfo(mDpy, res, oi->crtc)) {
                    if (ci->width > 0 && ci->height > 0) {
                        mPrimaryPx =
                            Rect(static_cast<float>(ci->x), static_cast<float>(ci->y),
                                 static_cast<float>(ci->width), static_cast<float>(ci->height));
                    }
                    XRRFreeCrtcInfo(ci);
                }
            }
            XRRFreeOutputInfo(oi);
        }
    }

    XRRFreeScreenResources(res);
}

//------------------------------------------------------------------------
// Everything that follows from the screen size, in the order its dependencies run.
void X11Window::applyGeometry()
{
    resolvePrimary();

    if (mAutoScale) {
        // From the PRIMARY OUTPUT's height, not the framebuffer's. The two differ in exactly
        // the cases where the difference matters: a dual-head machine whose framebuffer is
        // the bounding box of both screens, and a panning configuration where the framebuffer
        // is deliberately larger than the mode being displayed. In both, the framebuffer
        // height is not the height of the screen anybody is looking at, and scaling to it
        // gives a panel too big to fit on the one the panel is centred on.
        const int h = mPrimaryPx.h > 0.0f ? static_cast<int>(mPrimaryPx.h) : mPixelH;
        mScale = autoScale(h);
    }

    mLogicalW = static_cast<float>(mPixelW) / mScale;
    mLogicalH = static_cast<float>(mPixelH) / mScale;
}

//------------------------------------------------------------------------
// Re-read the screen size and make the window, the surface and the scale agree with it.
//
// THIS IS THE ONE THAT MATTERS ON THE LOGOUT PATH, and it is the bug it was written for: the
// X server outlives the session, so a desktop that sets 1920x1080 on a monitor whose preferred
// mode is 3840x2160 hands back a 1920x1080 screen when it exits. Nothing resized this window
// when that happened. The login screen then returns composed for the screen it started on --
// a panel scaled for 4K, centred where the middle of a 4K screen used to be, with only its
// top-left corner inside the part of it anybody can see. It reads as a zoomed-in login box
// with the entry fields off the bottom of the monitor.
//
// The size is read with XGetGeometry rather than DisplayWidth/DisplayHeight. Those return the
// Screen struct cached when the connection was set up, which only follows a resize if every
// path that reaches here has first handed the event to XRRUpdateConfiguration. A round trip
// to the server cannot be stale and does not depend on having seen the event at all -- which
// is what makes this correct on the return path even if RandR is missing entirely.
bool X11Window::syncScreenGeometry()
{
    if (!mDpy || !mWin)
        return false;

    ::Window rootRet = 0;
    int rx = 0, ry = 0;
    unsigned rw = 0, rh = 0, rborder = 0, rdepth = 0;
    if (!XGetGeometry(mDpy, mRoot, &rootRet, &rx, &ry, &rw, &rh, &rborder, &rdepth))
        return false;
    if (rw == 0 || rh == 0)
        return false;
    if (static_cast<int>(rw) == mPixelW && static_cast<int>(rh) == mPixelH)
        return false;

    mPixelW = static_cast<int>(rw);
    mPixelH = static_cast<int>(rh);

    XMoveResizeWindow(mDpy, mWin, 0, 0, rw, rh);
    // Before the surface is told its new size, because the resize is a request like any other
    // and Cairo would otherwise draw to a drawable the server has not grown yet.
    XSync(mDpy, False);

    // The surface caches the drawable's dimensions and clips to them; without this it keeps
    // clipping to the old size no matter what the drawable underneath it now is.
    // cites: cairo-xlib.h:63
    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, mPixelW, mPixelH);

    applyGeometry();
    mDirty = true;
    return true;
}

//------------------------------------------------------------------------
void X11Window::refreshGeometry()
{
    if (!syncScreenGeometry())
        return;
    if (mActive && mActive->resized)
        mActive->resized(bounds(), primaryBounds());
    else
        mResizePending = true;
}

//------------------------------------------------------------------------
// The input method, which is what makes dead keys, Compose and non-US layouts work.
//
// No input-method server runs before login -- there is no ibus, no fcitx, nobody's session has
// started -- so XOpenIM falls back to Xlib's built-in local input method. That is the one we
// want: it reads the X keyboard mapping and the Compose file and needs no daemon.
//
// The caller must have done setlocale() and XSetLocaleModifiers() before open(). If either the
// locale is unsupported or the IM will not open, mXic stays null and the key path falls back to
// XLookupString, which is Latin-1 and has no dead keys. That is a degradation, not a failure:
// somebody with an ASCII password can still get in and fix it.
void X11Window::openInputMethod()
{
    if (!XSupportsLocale()) {
        fprintf(stderr, "xlogin: X does not support the current locale; "
                        "dead keys and Compose will not work\n");
        return;
    }

    mXim = XOpenIM(mDpy, nullptr, nullptr, nullptr);
    if (!mXim) {
        fprintf(stderr, "xlogin: no X input method; dead keys and Compose will not work\n");
        return;
    }

    // XIMPreeditNothing | XIMStatusNothing is the style that needs no preedit or status window
    // of its own, which is the only style we can honour: there is nowhere to put one.
    mXic = XCreateIC(mXim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, mWin,
                     XNFocusWindow, mWin, nullptr);
    if (!mXic) {
        fprintf(stderr, "xlogin: could not create an input context; "
                        "dead keys and Compose will not work\n");
        XCloseIM(mXim);
        mXim = nullptr;
        return;
    }

    XSetICFocus(mXic);
}

void X11Window::closeInputMethod()
{
    if (mXic) {
        XDestroyIC(mXic);
        mXic = nullptr;
    }
    if (mXim) {
        XCloseIM(mXim);
        mXim = nullptr;
    }
}

//------------------------------------------------------------------------
void X11Window::takeFocus()
{
    if (!mDpy || !mWin)
        return;

    XRaiseWindow(mDpy, mWin);

    // XSetInputFocus against a window that is not viewable is a BadMatch, and the map request
    // above is asynchronous, so the state has to be read rather than assumed.
    XWindowAttributes wa;
    if (XGetWindowAttributes(mDpy, mWin, &wa) == 0 || wa.map_state != IsViewable)
        return;

    XSetInputFocus(mDpy, mWin, RevertToPointerRoot, CurrentTime);
    if (mXic)
        XSetICFocus(mXic);
    XFlush(mDpy);
}

//------------------------------------------------------------------------
// While a session runs this window must get out of the way. It is override-redirect and
// covers the screen, so leaving it mapped would put a login panel on top of the desktop; and
// there is no window manager to lower it for us, so lowering rather than unmapping would still
// leave it taking the pointer and the keyboard.
void X11Window::hide()
{
    if (!mDpy || !mWin)
        return;
    ungrabKeyboard();
    XUnmapWindow(mDpy, mWin);
    XFlush(mDpy);
}

void X11Window::show()
{
    if (!mDpy || !mWin)
        return;
    // Before the map, not after. The session that has just exited may have left the screen at
    // a size this window was never told about, and mapping first would put one frame of the
    // wrong thing on the screen before the correction arrived. It also makes the return path
    // correct without having had to receive the RandR event at all -- which matters, because
    // this is the path a user actually notices when it is wrong.
    refreshGeometry();
    XMapRaised(mDpy, mWin);
    // The map is asynchronous and takeFocus() refuses to focus a window that is not yet
    // viewable, so the round trip is needed before it, not after.
    XSync(mDpy, False);
    takeFocus();
    mDirty = true;
}

//------------------------------------------------------------------------
bool X11Window::grabKeyboard()
{
    if (!mDpy || !mWin)
        return false;
    if (mGrabbed)
        return true;

    for (int attempt = 0; attempt < kGrabAttempts; ++attempt) {
        // THE FOURTH ARGUMENT IS pointer_mode, AND IT MUST STAY GrabModeAsync.
        // XGrabKeyboard takes a pointer mode as well as a keyboard mode
        // (cites: Xlib.h:2730-2737, the argument order is
        //  display, grab_window, owner_events, pointer_mode, keyboard_mode, time).
        // GrabModeSync there FREEZES THE POINTER until the client calls XAllowEvents -- a
        // keyboard grab that silently kills the mouse. On this screen that would mean the
        // Options button, and therefore every way off this screen that is not the keyboard,
        // stops responding. The two GrabModeAsync arguments are not interchangeable
        // boilerplate; the first one is load-bearing.
        const int r = XGrabKeyboard(mDpy, mWin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        if (r == GrabSuccess) {
            mGrabbed = true;
            return true;
        }
        sleepNs(kGrabRetryNs);
    }

    // Deliberately not fatal. A login screen that will not accept typing is a worse outcome
    // than one typed without a grab, and the grab is a hardening measure against a local client
    // that, before anybody has logged in, should not exist. Typing still works without it,
    // because the input focus is set explicitly by takeFocus() and does not depend on the grab.
    //
    // Logged to syslog as well as stderr, and this is the reason: on tty1 stderr is underneath
    // the X server, so nobody ever sees it. Without the grab the password is readable by any
    // client that can reach the display, which is a security property quietly going missing --
    // exactly the kind of thing that must not be discoverable only by reading the code.
    fprintf(stderr, "xlogin: could not grab the keyboard; another client is holding it\n");
    syslog(LOG_AUTHPRIV | LOG_WARNING,
           "could not grab the keyboard after %d attempts; another client holds it. Keystrokes "
           "on this display are readable by other clients until it is released.",
           kGrabAttempts);
    return false;
}

bool X11Window::tryGrabKeyboard()
{
    if (!mDpy || !mWin)
        return false;
    if (mGrabbed)
        return true;

    if (XGrabKeyboard(mDpy, mWin, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) {
        mGrabbed = true;
        return true;
    }
    return false;
}

void X11Window::ungrabKeyboard()
{
    if (mDpy && mGrabbed) {
        XUngrabKeyboard(mDpy, CurrentTime);
        XFlush(mDpy);
        mGrabbed = false;
    }
}

//------------------------------------------------------------------------
void X11Window::addFd(int fd, std::function<void()> onReadable)
{
    if (fd < 0)
        return;
    removeFd(fd);
    mExtraFds.push_back(ExtraFd{fd, std::move(onReadable)});
}

void X11Window::removeFd(int fd)
{
    for (size_t i = 0; i < mExtraFds.size(); ++i) {
        if (mExtraFds[i].fd == fd) {
            mExtraFds.erase(mExtraFds.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

//------------------------------------------------------------------------
void X11Window::close()
{
    ungrabKeyboard();
    closeInputMethod();

    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mDpy) {
        if (mCursor) {
            XFreeCursor(mDpy, mCursor);
            mCursor = 0;
        }
        if (mWin) {
            XDestroyWindow(mDpy, mWin);
            mWin = 0;
        }
        unregisterDisplay(mDpy);
        XCloseDisplay(mDpy);
        mDpy = nullptr;
    }
}

//------------------------------------------------------------------------
void X11Window::paint(const Callbacks &cb)
{
    if (!cb.draw || !mTarget)
        return;

    // Compose offscreen. ARGB32 is premultiplied -- the one Cairo convention not pinned in
    // Canvas, because it belongs to whoever creates the surface, which is here.
    cairo_surface_t *buf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, mPixelW, mPixelH);
    if (cairo_surface_status(buf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(buf);
        return;
    }

    cairo_t *cr = cairo_create(buf);
    // ONE scale, here. Nothing downstream of this knows the scale exists.
    cairo_scale(cr, mScale, mScale);
    {
        Canvas canvas(cr, &mFonts, mLogicalW, mLogicalH);
        cb.draw(canvas);
    }
    cairo_destroy(cr);

    // Blit once, SOURCE not OVER: the buffer is the frame, not a layer on top of the last one.
    cairo_t *out = cairo_create(mTarget);
    cairo_set_source_surface(out, buf, 0, 0);
    cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
    cairo_paint(out);
    cairo_destroy(out);
    cairo_surface_destroy(buf);

    cairo_surface_flush(mTarget);
    XFlush(mDpy);
}

//------------------------------------------------------------------------
void X11Window::run(const Callbacks &cb)
{
    if (!mDpy || !mWin)
        return;

    mRunning = true;
    mDirty = true;
    mActive = &cb;

    // A resize that happened before there was anybody to tell. Delivering it here rather than
    // dropping it is what stops the loop opening with a layout for the wrong screen.
    if (mResizePending) {
        mResizePending = false;
        if (cb.resized)
            cb.resized(bounds(), primaryBounds());
    }

    const int xfd = ConnectionNumber(mDpy);

    while (mRunning) {
        // Drain everything the server has for us first, setting state but never painting: a
        // drag generates a MotionNotify per pixel and each one would otherwise be a full
        // recompose.
        while (XPending(mDpy)) {
            XEvent ev;
            XNextEvent(mDpy, &ev);

            // The input method gets first refusal. A dead-key press, and every key that makes
            // up a Compose sequence, is consumed here and reappears later as the composed
            // character on the key that completes it. Skipping this is why a naive X client
            // cannot type an umlaut.
            if (XFilterEvent(&ev, None))
                continue;

            // Before the switch, because an extension's event codes are handed out at
            // runtime and cannot be case labels. cites: randr.h:130
            if (mRandr && ev.type == mRREventBase + RRScreenChangeNotify) {
                // Brings Xlib's cached idea of the screen size back in step with the server's.
                // cites: Xrandr.h:446-450
                XRRUpdateConfiguration(&ev);
                refreshGeometry();
                continue;
            }

            switch (ev.type) {
                case Expose:
                    mDirty = true;
                    break;

                case ConfigureNotify:
                    // Our own window changed shape. Normally that is our own
                    // XMoveResizeWindow coming back, in which case the re-read finds nothing
                    // to do and this costs one round trip; but it is also the only notice we
                    // get on a server whose RandR did not answer.
                    if (ev.xconfigure.window == mWin)
                        refreshGeometry();
                    break;

                case FocusOut:
                    // Nothing should be able to take the focus -- there is no window manager
                    // and no other client that ought to exist yet -- so if something did, take
                    // it back rather than silently stop accepting keystrokes.
                    //
                    // BUT NOT EVERY FocusOut IS A LOST FOCUS. A keyboard grab generates one
                    // with mode NotifyGrab, and releasing it generates one with NotifyUngrab
                    // (cites: X11/X.h:268-269) -- so our OWN grabKeyboard() and
                    // ungrabKeyboard() each produce a FocusOut here. Acting on those means
                    // answering our own grab with an XSetInputFocus, every time, for nothing.
                    //
                    // It is worse than noise in the case that actually bites: if another
                    // client is taking the keyboard -- which is how this failed in a sibling
                    // project, AlreadyGrabbed on every attempt because a desktop held it --
                    // then every grab and ungrab IT does lands here too, and we answer each
                    // one by snatching the focus back while it still holds the grab. That is
                    // a focus fight with a client we cannot win against, and it presents as a
                    // screen that will not respond.
                    //
                    // Only a real transition is worth reacting to. NotifyPointer is likewise
                    // a pointer-crossing pseudo-focus (X.h:281), not a focus change.
                    if ((ev.xfocus.mode == NotifyNormal || ev.xfocus.mode == NotifyWhileGrabbed) &&
                        ev.xfocus.detail != NotifyPointer) {
                        takeFocus();
                    }
                    break;

                case ButtonPress:
                case ButtonRelease:
                    if (ev.xbutton.button == Button1 && cb.button) {
                        cb.button(static_cast<float>(ev.xbutton.x) / mScale,
                                  static_cast<float>(ev.xbutton.y) / mScale,
                                  ev.type == ButtonPress);
                    }
                    break;

                case MotionNotify:
                    if (cb.motion) {
                        cb.motion(static_cast<float>(ev.xmotion.x) / mScale,
                                  static_cast<float>(ev.xmotion.y) / mScale);
                    }
                    break;

                case LeaveNotify:
                    // A pointer that left without a ButtonRelease would otherwise leave a
                    // control latched in its hover state.
                    if (cb.motion)
                        cb.motion(-1.0f, -1.0f);
                    break;

                case KeyPress: {
                    KeySym sym = NoSymbol;
                    char buf[64];
                    int len = 0;

                    if (mXic) {
                        Status status = XLookupNone;
                        len =
                            Xutf8LookupString(mXic, &ev.xkey, buf, sizeof(buf) - 1, &sym, &status);
                        // XLookupBoth and XLookupChars carry text; XLookupKeySym carries only
                        // the symbol. Anything else produced neither.
                        if (status != XLookupChars && status != XLookupBoth)
                            len = 0;
                        if (status != XLookupKeySym && status != XLookupBoth)
                            sym = NoSymbol;
                    } else {
                        // No input method: Latin-1 only, no dead keys. Warned about at open().
                        len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &sym, nullptr);
                        if (len < 0)
                            len = 0;
                    }

                    if (len < 0 || len > static_cast<int>(sizeof(buf) - 1))
                        len = 0;
                    buf[len] = '\0';

                    if (cb.key)
                        cb.key(sym, buf, len, ev.xkey.state);
                    break;
                }

                default:
                    break;
            }
        }

        if (!mRunning)
            break;

        if (mDirty) {
            mDirty = false;
            paint(cb);
        }

        // Wait for the next event, an extra fd, or the next tick, whichever comes first.
        // XPending above may have left events buffered inside Xlib that never reach the fd, so
        // it is checked again rather than slept through.
        if (XPending(mDpy))
            continue;

        fd_set r;
        FD_ZERO(&r);
        FD_SET(xfd, &r);
        int maxFd = xfd;
        for (const ExtraFd &e : mExtraFds) {
            FD_SET(e.fd, &r);
            if (e.fd > maxFd)
                maxFd = e.fd;
        }

        struct timeval tv;
        tv.tv_sec = mTickMs / 1000;
        tv.tv_usec = (mTickMs % 1000) * 1000;

        const int n = select(maxFd + 1, &r, nullptr, nullptr, &tv);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "xlogin: select on the X connection failed; closing\n");
            mRunning = false;
        } else if (n == 0) {
            if (cb.tick)
                cb.tick();
        } else {
            // A copy, because a handler is allowed to call addFd/removeFd.
            const std::vector<ExtraFd> snapshot = mExtraFds;
            for (const ExtraFd &e : snapshot) {
                if (FD_ISSET(e.fd, &r) && e.onReadable)
                    e.onReadable();
            }
        }
    }

    mActive = nullptr;
}

} // namespace xlogin
