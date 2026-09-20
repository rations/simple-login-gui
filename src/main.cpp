// xlogin -- a graphical login for Devuan on sysvinit and seatd.
//
// Wiring only. Everything of substance is in one of four places: the window (platform/), the
// drawing (gfx/), the layout (ui/panel.cpp) and the root-privileged half (session/, plain C).
// This file opens them, connects them and owns the login state machine, which is small:
//
//     Idle  --submit-->  Authenticating  --ok-->  Session running  --child exits-->  Idle
//                              |
//                              +--fail--> Idle, with a message
//
// WHAT IS DONE BEFORE ANYTHING ELSE, AND WHY IT IS FIRST:
//
//   * prctl(PR_SET_DUMPABLE, 0). This process is root and will hold a plaintext password. A
//     core file of it is the worst possible artifact of a crash, so the ability to write one
//     is given up before there is anything worth dumping.
//   * setlocale + XSetLocaleModifiers, BEFORE the window opens. XOpenIM reads the locale that
//     was current when the display was opened; doing this afterwards silently gives an input
//     method that cannot compose, and the symptom is an accented password that cannot be
//     typed -- months later, by somebody else.
//   * The signal handlers and their self-pipe, before the loop can be entered.
//
// SIGNALS GO THROUGH A SELF-PIPE. A handler does one async-signal-safe write() and nothing
// else; the loop reads the pipe as an ordinary file descriptor alongside the X connection.
// That is what the GLib implementation this replaces got from g_unix_signal_add and
// g_child_watch_add, and it is why the event loop never blocks waiting for a child: the window
// keeps turning and repainting for the whole life of the session.

#include "geometry.h"
#include "gfx/image.h"
#include "gfx/menu.h"
#include "platform/keymap.h"
#include "platform/respath.h"
#include "platform/x11window.h"
#include "ui/panel.h"

extern "C" {
#include "config.h"
#include "session/auth.h"
#include "session/cleanup.h"
#include "session/launch.h"
#include "session/power.h"
}

#include <X11/Xlib.h>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xlogin;

namespace
{

//------------------------------------------------------------------------
// The self-pipe. Global because a signal handler cannot be given a context.
int gSigPipe[2] = {-1, -1};

// Async-signal-safe: one write() of one byte and nothing else. Not fprintf, not malloc, not a
// std::function -- a handler that allocates can deadlock against the allocator it interrupted.
void onSignal(int sig)
{
    const unsigned char b = static_cast<unsigned char>(sig);
    // The result is deliberately discarded. A full pipe means a byte is already waiting, which
    // is all the loop needs to know, and there is nothing useful a handler could do about an
    // error anyway.
    const ssize_t n = write(gSigPipe[1], &b, 1);
    (void)n;
}

bool installSignalHandlers()
{
    if (pipe2(gSigPipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        fprintf(stderr, "xlogin: could not create the signal pipe: %s\n", strerror(errno));
        return false;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART so that a signal arriving mid-read does not turn into an EINTR somewhere that
    // does not expect one. select() is handled explicitly in the loop either way.
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGCHLD};
    for (int s : signals) {
        if (sigaction(s, &sa, nullptr) != 0) {
            fprintf(stderr, "xlogin: could not install a handler for signal %d: %s\n", s,
                    strerror(errno));
            return false;
        }
    }

    // A client that dies mid-write to the X connection would otherwise take this process with
    // it, and the X connection is exactly the sort of thing that goes away unexpectedly here.
    signal(SIGPIPE, SIG_IGN);
    return true;
}

//------------------------------------------------------------------------
std::string hostName()
{
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0)
        return "this machine";
    buf[sizeof(buf) - 1] = '\0';
    return buf[0] ? std::string(buf) : std::string("this machine");
}

//------------------------------------------------------------------------
// The login state machine. One object so that the callbacks do not become a pile of globals.
class App
{
public:
    explicit App(X11Window &win) : mWin(win)
    {
    }

    void setUp()
    {
        config_load(&mCfg);

        mPanel.layout(mWin.bounds(), mWin.primaryBounds());
        mPanel.setHostname(hostName());
        applyBackground();
        mPanel.reset();

        mPanel.cb.submit = [this] { submit(); };
        mPanel.cb.options = [this] { options(); };
        mPanel.cb.menuAction = [this](const MenuItem &item) { menuAction(item); };

        // Warn once, at startup, rather than at the moment somebody needs the console: a VT
        // with no getty on it shows a black screen with a cursor, which reads as a crash.
        if (power_vt_has_getty(mCfg.console_vt) == 0) {
            fprintf(stderr,
                    "xlogin: no getty appears to respawn on tty%d, so the Console entry may "
                    "leave a blank screen; check /etc/inittab\n",
                    mCfg.console_vt);
        }
    }

    ~App()
    {
        if (mBackground)
            cairo_surface_destroy(mBackground);
    }

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    Panel &panel()
    {
        return mPanel;
    }

    //--- the loop's callbacks ------------------------------------------
    void draw(Canvas &c)
    {
        mPanel.draw(c);
    }

    void motion(float x, float y)
    {
        mPanel.motion(x, y);
        mWin.invalidate();
    }

    // Once every tick. The only thing that happens here is retaking the keyboard after a VT
    // switch: there is no event that says "the user came back from tty2", and polling a
    // single non-blocking attempt four times a second costs nothing.
    void tick()
    {
        if (mSessionPid > 0 || mWin.keyboardGrabbed())
            return;
        if (mWin.tryGrabKeyboard())
            mWin.invalidate();
    }

    void button(float x, float y, bool pressed)
    {
        if (mSessionPid > 0)
            return; // the panel is not on the screen; nothing here is clickable
        mPanel.click(x, y, pressed);
        mWin.invalidate();
    }

    bool key(KeySym sym, const char *text, int len, unsigned /*state*/)
    {
        if (mSessionPid > 0)
            return false;
        mPanel.key(keyFromSym(sym), text, len);
        mWin.invalidate();
        return true;
    }

    // Called when the self-pipe has bytes: one per signal delivered.
    void onSignalByte(int sig)
    {
        switch (sig) {
            case SIGCHLD:
                reapSession();
                break;
            case SIGTERM:
            case SIGINT:
            case SIGHUP:
                // Only when no session is running. A stray SIGTERM must not take the user's
                // desktop down with it, and while a session runs this process's job is to sit
                // out of the way and wait for it.
                if (mSessionPid <= 0) {
                    fprintf(stderr, "xlogin: signal %d, exiting\n", sig);
                    mWin.stop();
                }
                break;
            default:
                break;
        }
    }

private:
    //--- submit --------------------------------------------------------
    void submit()
    {
        if (mSessionPid > 0)
            return;

        TextField &user = mPanel.username();
        TextField &pass = mPanel.password();

        if (user.empty() || pass.empty()) {
            mPanel.setStatus("Enter a username and password", true);
            mWin.invalidate();
            return;
        }

        // Keep the name: the session launch and the cleanup afterwards both need it, and the
        // field is about to be disabled and may be cleared.
        mUser = user.text();

        mPanel.setEnabled(false);
        mPanel.setStatus("Authenticating...", false);
        // The one deliberate synchronous repaint. pam_authenticate can block for seconds
        // against a slow module, and a screen still showing the old status for those seconds
        // reads as a hang.
        mWin.paintNow();

        const auth_result r = auth_login(mUser.c_str(), pass.text());

        // Erase the password the moment PAM has finished with it, on BOTH paths. There is no
        // reason for the plaintext to outlive the call that needed it, and "we will clear it
        // when the session ends" is how it ends up in a core file.
        pass.clear();

        if (!r.ok) {
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "authentication failed for user %s", mUser.c_str());
            // pam_strerror's text and nothing more. Which of the username and the password was
            // wrong is not something to tell somebody who has not proved who they are.
            mPanel.setStatus(r.message, true);
            mPanel.setEnabled(true);
            mWin.invalidate();
            return;
        }

        startSession();
    }

    void startSession()
    {
        mPanel.setStatus("Starting session...", false);
        mWin.paintNow();

        // Give the keyboard back before the session's clients start asking for it, and get
        // the window off the screen so the session is visible.
        mWin.ungrabKeyboard();
        mWin.hide();

        const launch_result lr = launch_session(mUser.c_str());
        if (!lr.ok) {
            syslog(LOG_AUTHPRIV | LOG_ERR, "could not start a session for %s: %s", mUser.c_str(),
                   lr.message);
            auth_close_session();
            mWin.show();
            mWin.grabKeyboard();
            mPanel.setEnabled(true);
            mPanel.setStatus(lr.message, true);
            mWin.invalidate();
            return;
        }

        mSessionPid = lr.pid;
        syslog(LOG_AUTHPRIV | LOG_INFO, "session started for %s (pid %ld)", mUser.c_str(),
               static_cast<long>(mSessionPid));
    }

    //--- the session ended ---------------------------------------------
    void reapSession()
    {
        // WNOHANG in a loop: one SIGCHLD can stand for several children, and signals do not
        // queue. Reaping everything that is ready is the only way not to leave a zombie.
        int status = 0;
        pid_t p;
        bool ours = false;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            if (p == mSessionPid)
                ours = true;
        }
        if (!ours || mSessionPid <= 0)
            return;

        syslog(LOG_AUTHPRIV | LOG_INFO, "session ended for %s", mUser.c_str());
        mSessionPid = -1;

        // PAM first, so the session is closed at the seat before its leftovers are killed.
        auth_close_session();
        // Then whatever the session script left connected to the display: a panel, a
        // compositor, a wallpaper setter. They would draw over the login screen otherwise.
        cleanup_after_session(mUser.c_str());

        // The root window does not need clearing the way it used to. The old implementation
        // forked xsetroot for that, because a wallpaper setter leaves a pixmap on the root
        // window that outlives the process; this window is fullscreen and opaque and covers
        // it, so the fork and the dependency both go away.
        mWin.show();
        mWin.grabKeyboard();
        mPanel.reset();
        mWin.invalidate();
    }

    //--- the Options menu ----------------------------------------------
    void options()
    {
        if (mPanel.menuIsOpen()) {
            mPanel.closeMenu();
            mWin.invalidate();
            return;
        }
        showMainMenu();
    }

    void showMainMenu()
    {
        std::vector<MenuItem> items;

        MenuItem console;
        console.label = "Console (tty" + std::to_string(mCfg.console_vt) + ")";
        console.action = MenuAction::Console;
        items.push_back(console);

        MenuItem bg;
        bg.label = "Background...";
        bg.action = MenuAction::ShowBackgrounds;
        items.push_back(bg);

        // The two that cannot be undone, in a group of their own below a separator, so that
        // neither is next to the entry somebody reaches for most often.
        MenuItem restart;
        restart.label = "Restart";
        restart.action = MenuAction::Restart;
        restart.destructive = true;
        restart.startsGroup = true;
        items.push_back(restart);

        MenuItem shutdown;
        shutdown.label = "Shut down";
        shutdown.action = MenuAction::Shutdown;
        shutdown.destructive = true;
        items.push_back(shutdown);

        MenuItem close;
        close.label = "Close";
        close.action = MenuAction::Dismiss;
        close.startsGroup = true;
        items.push_back(close);

        mPanel.setMenuItems(std::move(items));
        mPanel.openMenu();
        mWin.invalidate();
    }

    void showBackgroundMenu()
    {
        std::vector<MenuItem> items;

        MenuItem back;
        back.label = "< Back";
        back.action = MenuAction::BackToMain;
        items.push_back(back);

        MenuItem none;
        none.label = "(none)";
        none.action = MenuAction::SetBackground;
        none.value = "";
        none.startsGroup = true;
        items.push_back(none);

        // Only what is in the one root-owned directory, and only what passed the ownership
        // checks in image.cpp. There is no file browser and there is not going to be one:
        // letting an unauthenticated person at the keyboard enumerate the filesystem as root
        // is a larger concession than a wallpaper picker is worth.
        const std::vector<std::string> names = listBackgroundImages(XLOGIN_BACKGROUND_DIR);
        for (const std::string &n : names) {
            MenuItem item;
            item.label = n;
            item.action = MenuAction::SetBackground;
            item.value = n;
            items.push_back(item);
        }

        if (names.empty()) {
            MenuItem empty;
            empty.label = "(no images installed)";
            empty.action = MenuAction::Nothing;
            items.push_back(empty);
        }

        mPanel.setMenuItems(std::move(items));
        mPanel.openMenu();
        mWin.invalidate();
    }

    void menuAction(const MenuItem &item)
    {
        const char *msg = "";

        switch (item.action) {
            case MenuAction::Nothing:
                return;

            case MenuAction::Dismiss:
                mPanel.closeMenu();
                mWin.invalidate();
                return;

            case MenuAction::ShowBackgrounds:
                showBackgroundMenu();
                return;

            case MenuAction::BackToMain:
                showMainMenu();
                return;

            case MenuAction::SetBackground:
                setBackground(item.value);
                return;

            case MenuAction::Console:
                mPanel.closeMenu();
                // The grab has to go before the switch, or this process is holding the
                // keyboard on a VT nobody is looking at. tick() takes it back when the user
                // returns with Alt+F1.
                mWin.ungrabKeyboard();
                if (power_switch_console(mCfg.console_vt, &msg) != 0) {
                    mWin.grabKeyboard();
                    mPanel.setStatus(msg, true);
                }
                mWin.invalidate();
                return;

            case MenuAction::Shutdown:
                mPanel.closeMenu();
                mPanel.setStatus("Shutting down...", false);
                mWin.paintNow();
                if (power_shutdown(&msg) != 0) {
                    syslog(LOG_AUTHPRIV | LOG_ERR, "shutdown failed: %s", msg);
                    mPanel.setStatus(msg, true);
                }
                mWin.invalidate();
                return;

            case MenuAction::Restart:
                mPanel.closeMenu();
                mPanel.setStatus("Restarting...", false);
                mWin.paintNow();
                if (power_restart(&msg) != 0) {
                    syslog(LOG_AUTHPRIV | LOG_ERR, "restart failed: %s", msg);
                    mPanel.setStatus(msg, true);
                }
                mWin.invalidate();
                return;
        }
    }

    //--- the background ------------------------------------------------
    void applyBackground()
    {
        if (mBackground) {
            cairo_surface_destroy(mBackground);
            mBackground = nullptr;
        }
        if (mCfg.background[0])
            mBackground = loadBackgroundImage(XLOGIN_BACKGROUND_DIR, mCfg.background);
        // A null surface is the flat ground, which is exactly what a refused or unreadable
        // image must degrade to. drawBackground treats it as a no-op.
        mPanel.setBackground(mBackground, bgModeFromString(mCfg.bg_mode));
    }

    void setBackground(const std::string &name)
    {
        snprintf(mCfg.background, sizeof(mCfg.background), "%s", name.c_str());
        applyBackground();

        if (!name.empty() && !mBackground) {
            // listBackgroundImages only sniffs each file's first eight bytes, so a truncated
            // or oversized image can be offered and then fail to load. Say so, and leave the
            // flat ground up rather than writing a setting that does not work.
            mPanel.setStatus("That image could not be loaded", true);
            mPanel.closeMenu();
            mWin.invalidate();
            return;
        }

        if (config_set("XLOGIN_BACKGROUND", mCfg.background) != 0)
            mPanel.setStatus("Background applied, but could not be saved", true);
        else
            mPanel.clearStatus();

        mPanel.closeMenu();
        mWin.invalidate();
    }

    X11Window &mWin;
    Panel mPanel;
    xlogin_config mCfg = {};
    cairo_surface_t *mBackground = nullptr;
    std::string mUser;
    pid_t mSessionPid = -1;
};

} // namespace

//------------------------------------------------------------------------
int main()
{
    // FIRST. Before a password can exist in this address space.
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
        fprintf(stderr, "xlogin: could not disable core dumps: %s\n", strerror(errno));

    openlog("xlogin", LOG_PID, LOG_AUTHPRIV);

    // BEFORE the display is opened: XOpenIM reads the locale that was current at that point.
    if (!setlocale(LC_ALL, ""))
        fprintf(stderr, "xlogin: the system locale is not supported; falling back to C\n");
    if (!XSetLocaleModifiers(""))
        fprintf(stderr, "xlogin: could not set the X locale modifiers\n");

    if (!installSignalHandlers())
        return 1;

    X11Window win;
    // scale 0 = choose from the screen height. 250 ms tick: nothing animates, so this is only
    // how long the loop may sleep before noticing something it was not woken for.
    if (!win.open("xlogin", 0.0f, 250))
        return 1;

    App app(win);
    app.setUp();

    // Not fatal if it fails -- grabKeyboard has already said so -- because a login screen that
    // will not accept typing is a worse outcome than one typed without a grab.
    win.grabKeyboard();

    win.addFd(gSigPipe[0], [&app] {
        unsigned char buf[64];
        ssize_t n;
        while ((n = read(gSigPipe[0], buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; ++i)
                app.onSignalByte(buf[i]);
        }
    });

    X11Window::Callbacks cb;
    cb.draw = [&app](Canvas &c) { app.draw(c); };
    cb.motion = [&app](float x, float y) { app.motion(x, y); };
    cb.button = [&app](float x, float y, bool pressed) { app.button(x, y, pressed); };
    cb.key = [&app](KeySym sym, const char *text, int len, unsigned state) {
        return app.key(sym, text, len, state);
    };
    cb.tick = [&app] { app.tick(); };

    win.run(cb);

    // The password field's destructor erases its buffer, but the panel outlives this scope
    // only until App does, and an explicit clear here means the erase does not depend on
    // destruction order.
    app.panel().password().clear();
    auth_close_session();
    closelog();
    return 0;
}
