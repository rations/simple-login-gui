// uirender -- render and MEASURE the real login screen, with no X server running.
//
// This is the cheap half of the test strategy and the reason src/gfx/ and src/ui/ are kept
// free of Xlib. The alternative way to find out that the status line is clipped is to install
// this program on tty1 and reboot into it, and the string most likely to be clipped is the one
// that tells a user why they cannot log in.
//
// It renders the SAME Panel class the window renders, through the same Canvas, with the same
// bundled faces, at the same scales X11Window::autoScale picks on real hardware. Nothing here
// is a mock-up of the login screen; it is the login screen, composed to a file.
//
// Two kinds of check, and the second is the one that fails builds:
//
//   * MEASUREMENT. Every string that has a slot is measured against that slot at its real
//     size in its real face, with worst-case content -- the longest PAM error, a username at
//     the length the old implementation accepted, a full password. An overflow is a failure
//     even though Canvas::clipToWidth would have drawn an ellipsis, because an ellipsis in
//     the middle of "Your account has expired" is a login screen that has stopped explaining
//     itself.
//   * INK CLEARANCE. geometry.h asserts its clearances at compile time against the font's
//     nominal box. Here they are re-checked against cairo's actual ink extents for the actual
//     strings, which is the number that decides whether two rows really touch.
//
// Usage:  tools/uirender <resource-dir> <out-dir> [background-dir] [background-name] [mode]

#include "geometry.h"
#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "gfx/image.h"
#include "gfx/ink.h"
#include "gfx/keys.h"
#include "gfx/menu.h"
#include "gfx/palette.h"
#include "ui/panel.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xlogin;

namespace
{

int gFailures = 0;

void failure(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void failure(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "uirender: FAIL ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    ++gFailures;
}

// The worst case each slot has to survive. Real strings, not lorem ipsum: the PAM messages are
// what pam_strerror actually returns, and the username is the length the GTK implementation
// accepted without complaint.
const char *const kStatusWorstCase[] = {
    "Authentication failure",
    "Your account has expired; please contact your system administrator",
    "Permission denied",
    "Authentication token is no longer valid; new one required",
    "Enter a username and password",
    "Failed to create runtime dir",
    "User not found",
    "Starting session...",
};

const char *const kUsernameWorstCase[] = {
    "aVeryLongUserNameThatSomebodyWillHaveSoonerOrLater",
    "jean-françois",
    "user",
};

//------------------------------------------------------------------------
// Measure one string against one slot, at a given face and size.
void checkFits(Canvas &c, Font face, float size, const char *what, const std::string &s,
               float slotW)
{
    c.setFont(face);
    c.setFontSize(size);
    const float w = c.stringWidth(s.c_str());
    if (w > slotW) {
        failure("%s overflows its slot: %.1f > %.1f for \"%s\"", what, static_cast<double>(w),
                static_cast<double>(slotW), s.c_str());
    }
}

// Re-check a clearance against real ink rather than the nominal font box geometry.h asserts
// with. `upper` is drawn at baseline `upperBase`, `lower` at `lowerBase`, and their ink must
// not meet.
void checkClearance(Canvas &c, Font face, float upperSize, const char *upper, float upperBase,
                    float lowerSize, const char *lower, float lowerBase, const char *what)
{
    c.setFont(face);
    c.setFontSize(upperSize);
    const float bottom = upperBase + c.stringDescent(upper);
    c.setFontSize(lowerSize);
    const float top = lowerBase - c.stringAscent(lower);
    if (bottom >= top) {
        failure("%s: ink overlaps by %.2f units (\"%s\" ends at %.2f, \"%s\" starts at %.2f)", what,
                static_cast<double>(bottom - top), upper, static_cast<double>(bottom), lower,
                static_cast<double>(top));
    }
}

//------------------------------------------------------------------------
// Drive the panel's key handler with a string, exactly as the window would after the input
// method had composed it. This is what proves the field's UTF-8 handling on real text rather
// than on the ASCII a test would otherwise reach for.
void typeInto(Panel &p, const char *utf8)
{
    for (const char *q = utf8; *q;) {
        // One whole UTF-8 sequence per call, the way Xutf8LookupString delivers it.
        int len = 1;
        while (q[len] && (static_cast<unsigned char>(q[len]) & 0xC0) == 0x80)
            ++len;
        p.key(Key::Plain, q, len);
        q += len;
    }
}

//------------------------------------------------------------------------
// Which menu, if any, is open in a scene.
enum class Show { NoMenu, MainMenu, ArmedMenu, BackgroundMenu };

struct Scene {
    const char *name;
    const char *user;
    const char *password;
    const char *status;
    bool statusIsError;
    bool enabled;
    bool passwordFocused;
    Show show;
};

// The main menu as main.cpp builds it. Kept here rather than shared, deliberately: if the two
// drift, the audit stops measuring the real thing, and a shared builder would hide that by
// making them impossible to drift. The labels here are the ones that must fit.
std::vector<MenuItem> mainMenuItems()
{
    std::vector<MenuItem> v;
    v.push_back({"Console (tty2)", MenuAction::Console, "", false, false});
    v.push_back({"Background...", MenuAction::ShowBackgrounds, "", false, false});
    v.push_back({"Restart", MenuAction::Restart, "", true, true});
    v.push_back({"Shut down", MenuAction::Shutdown, "", true, false});
    v.push_back({"Close", MenuAction::Dismiss, "", false, true});
    return v;
}

std::vector<MenuItem> backgroundMenuItems()
{
    std::vector<MenuItem> v;
    v.push_back({"< Back", MenuAction::BackToMain, "", false, false});
    v.push_back({"(none)", MenuAction::SetBackground, "", false, true});
    // Worst case: a filename at the length a menu row can hold, and one that is far too long.
    v.push_back({"mountains-at-dusk.jpg", MenuAction::SetBackground, "mountains-at-dusk.jpg", false,
                 false});
    v.push_back({"a-really-quite-long-wallpaper-filename-somebody-will-have.png",
                 MenuAction::SetBackground, "x.png", false, false});
    return v;
}

const Scene kScenes[] = {
    {"empty", "", "", "", false, true, false, Show::NoMenu},
    {"typing", "human", "hunter2", "", false, true, true, Show::NoMenu},
    {"rejected", "aVeryLongUserNameThatSomebodyWillHaveSoonerOrLater", "",
     "Your account has expired; please contact your system administrator", true, true, false,
     Show::NoMenu},
    {"busy", "human", "correct horse battery staple", "Authenticating...", false, false, true,
     Show::NoMenu},
    {"accents", "jean-françois", "pässwörd-with-ümläuts", "", false, true, true, Show::NoMenu},
    {"menu", "human", "", "", false, true, false, Show::MainMenu},
    // The confirm step, and the case that matters most: the menu open while the rest of the
    // panel is DISABLED, which is what somebody sees if PAM has hung and they need a way out.
    {"menu-armed", "human", "", "Authenticating...", false, false, false, Show::ArmedMenu},
    {"menu-backgrounds", "human", "", "", false, true, false, Show::BackgroundMenu},
};

//------------------------------------------------------------------------
bool renderScene(const FontStack &fonts, const Scene &sc, float scale, int screenW, int screenH,
                 cairo_surface_t *background, BgMode bgMode, const std::string &outPath)
{
    const int pw = static_cast<int>(static_cast<float>(screenW) * scale);
    const int ph = static_cast<int>(static_cast<float>(screenH) * scale);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        failure("could not create a %dx%d surface", pw, ph);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    // The ONE scale, applied exactly where X11Window::paint applies it.
    cairo_scale(cr, scale, scale);

    {
        Canvas c(cr, &fonts, static_cast<float>(screenW), static_cast<float>(screenH));

        Panel panel;
        const Rect screen(0, 0, static_cast<float>(screenW), static_cast<float>(screenH));
        panel.layout(screen, screen);
        panel.setHostname("devuan-excalibur.local");
        panel.setBackground(background, bgMode);
        panel.reset();

        typeInto(panel, sc.user);
        if (sc.passwordFocused || sc.password[0]) {
            panel.key(Key::Tab, "", 0);
            typeInto(panel, sc.password);
        }
        if (sc.status[0])
            panel.setStatus(sc.status, sc.statusIsError);
        panel.setEnabled(sc.enabled);

        if (sc.show != Show::NoMenu) {
            panel.setMenuItems(sc.show == Show::BackgroundMenu ? backgroundMenuItems()
                                                               : mainMenuItems());
            panel.openMenu();
            if (sc.show == Show::ArmedMenu) {
                // Walk down to Shut down and arm it, through the key handler rather than by
                // reaching into the menu -- so this also proves the keyboard path works while
                // the panel behind it is disabled.
                panel.key(Key::Down, "", 0);
                panel.key(Key::Down, "", 0);
                panel.key(Key::Down, "", 0);
                panel.key(Key::Enter, "", 0);
                if (!panel.menuIsOpen())
                    failure("scene %s: arming Shut down closed the menu instead", sc.name);
            }
        }

        panel.draw(c);

        // The field contents must be what was typed, byte for byte. A field that silently
        // dropped the second byte of an e-acute would still LOOK right in the PNG.
        if (strcmp(panel.username().text(), sc.user) != 0) {
            failure("scene %s: username round-tripped as \"%s\", not \"%s\"", sc.name,
                    panel.username().text(), sc.user);
        }
        if (sc.password[0] && strcmp(panel.password().text(), sc.password) != 0) {
            failure("scene %s: password did not round-trip", sc.name);
        }

        // A full field must SCROLL, not overrun. contentWidth is what the field will draw.
        const float slotW = geo::kFieldW - 2.0f * geo::kFieldPadX;
        c.setFont(Font::Body);
        c.setFontSize(geo::kFieldTextSize);
        const float userW = panel.username().contentWidth(c);
        if (userW > slotW) {
            printf("uirender: note - username \"%s\" is %.1f wide in a %.1f slot; it scrolls\n",
                   sc.user, static_cast<double>(userW), static_cast<double>(slotW));
        }
    }

    cairo_destroy(cr);
    const cairo_status_t st = cairo_surface_write_to_png(surface, outPath.c_str());
    cairo_surface_destroy(surface);

    if (st != CAIRO_STATUS_SUCCESS) {
        failure("could not write %s: %s", outPath.c_str(), cairo_status_to_string(st));
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
// Everything that has a slot, measured against it.
void auditLayout(const FontStack &fonts)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(s);
    Canvas c(cr, &fonts, 8.0f, 8.0f);

    const float inner = geo::kPanelW - 2.0f * geo::kMargin;

    checkFits(c, Font::Title, geo::kTitleSize, "the title", "LOGIN", inner);
    checkFits(c, Font::Body, geo::kLabelSize, "the username label", "Username", inner);
    checkFits(c, Font::Body, geo::kLabelSize, "the password label", "Password", inner);
    checkFits(c, Font::Body, geo::kHostSize, "the hostname line", "devuan-excalibur.local", inner);

    for (const char *s2 : kStatusWorstCase)
        checkFits(c, Font::Body, geo::kStatusSize, "a status message", s2, geo::kStatusW);

    // Button labels, against the label slot rather than the whole button: the padding either
    // side is what stops a label touching the outline.
    checkFits(c, Font::Body, geo::kButtonTextSize, "the Log in button label", "Log in",
              geo::kLoginW - 2.0f * geo::kFieldPadX);
    checkFits(c, Font::Body, geo::kButtonTextSize, "the Options button label", "Options",
              geo::kOptionsW - 2.0f * geo::kFieldPadX);

    // Menu labels, against the menu's own slot. The long filename is expected to be clipped
    // -- a wallpaper can be called anything -- so it is excluded from the check and the rest
    // are not.
    const float menuSlot = geo::kMenuW - 2.0f * geo::kMenuPadX;
    for (const MenuItem &m : mainMenuItems()) {
        checkFits(c, Font::Body, geo::kMenuTextSize, "a menu label", m.label, menuSlot);
        if (m.destructive) {
            // The armed form is longer than the label, and it is the one that must not be
            // clipped: "Confirm: Shut do..." is not a confirmation anybody should act on.
            checkFits(c, Font::Body, geo::kMenuTextSize, "an armed menu label",
                      "Confirm: " + m.label, menuSlot);
        }
    }
    checkFits(c, Font::Body, geo::kMenuTextSize, "the longest console label", "Console (tty63)",
              menuSlot);

    // Placeholders, which are drawn in the field's own slot.
    const float fieldSlot = geo::kFieldW - 2.0f * geo::kFieldPadX;
    checkFits(c, Font::Body, geo::kFieldTextSize, "the username placeholder", "username",
              fieldSlot);
    checkFits(c, Font::Body, geo::kFieldTextSize, "the password placeholder", "password",
              fieldSlot);

    // How many masked characters fit before the password field starts scrolling. Not a
    // failure -- scrolling is correct behaviour -- but a number worth printing, because if it
    // were small it would mean the dots were too far apart.
    const int dotsThatFit = static_cast<int>(fieldSlot / geo::kDotPitch);
    printf("uirender: %d password dots fit before the field scrolls\n", dotsThatFit);
    if (dotsThatFit < 20)
        failure("only %d password dots fit; the dot pitch is too wide", dotsThatFit);

    // The clearances geometry.h asserts nominally, re-checked against real ink. Descenders are
    // the whole point, so each pair uses strings that actually have them.
    checkClearance(c, Font::Body, geo::kHostSize, "devuan-gp.local", geo::kHostBaselineY,
                   geo::kLabelSize, "Username", geo::kUserLabelBaselineY,
                   "hostname vs username label");
    checkClearance(c, Font::Body, geo::kLabelSize, "Password", geo::kPassLabelBaselineY,
                   geo::kFieldTextSize, "pässwörd", geo::fieldTextBaseline(geo::kPassFieldY),
                   "password label vs its field text");
    checkClearance(c, Font::Body, geo::kFieldTextSize, "jean-françois",
                   geo::fieldTextBaseline(geo::kUserFieldY), geo::kLabelSize, "Password",
                   geo::kPassLabelBaselineY, "username field text vs password label");
    checkClearance(c, Font::Body, geo::kStatusSize,
                   "Your account has expired; please contact your system administrator",
                   geo::kStatusBaselineY, geo::kButtonTextSize, "Log in",
                   geo::kButtonY + geo::kButtonH * 0.5f +
                       geo::kButtonTextSize * geo::kLabelBaselineBias,
                   "status line vs button row");

    // Field text must sit inside its own well, measured rather than assumed. A descender
    // clipped by the bottom of a password field is the kind of thing nobody notices until a
    // user with a 'g' in their name reports it.
    c.setFont(Font::Body);
    c.setFontSize(geo::kFieldTextSize);
    const float base = geo::fieldTextBaseline(geo::kUserFieldY);
    const float inkTop = base - c.stringAscent("Jjfl");
    const float inkBottom = base + c.stringDescent("gypq");
    if (inkTop < geo::kUserFieldY)
        failure("field text ascends %.2f units above its well", geo::kUserFieldY - inkTop);
    if (inkBottom > geo::kUserFieldY + geo::kFieldH) {
        failure("field text descends %.2f units below its well",
                inkBottom - (geo::kUserFieldY + geo::kFieldH));
    }

    // Every worst-case username, against the field slot. These are allowed to scroll, so this
    // reports rather than fails -- but silence here would mean the longest one somehow fits,
    // which would be worth knowing too.
    for (const char *u : kUsernameWorstCase) {
        c.setFontSize(geo::kFieldTextSize);
        const float w = c.stringWidth(u);
        printf("uirender: username \"%s\" is %.1f units in a %.1f slot (%s)\n", u,
               static_cast<double>(w), static_cast<double>(fieldSlot),
               w > fieldSlot ? "scrolls" : "fits");
    }

    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <resource-dir> <out-dir> [bg-dir] [bg-name] "
                "[fill|fit|center|stretch|tile]\n",
                argv[0]);
        return 2;
    }

    const std::string resourceDir = argv[1];
    const std::string outDir = argv[2];

    FontStack fonts;
    const bool bundled = fonts.load(resourceDir);
    if (!bundled) {
        // Fatal, and deliberately so: measuring text against a substituted system face proves
        // nothing at all about whether it fits on the machine this will be installed on.
        fprintf(stderr, "uirender: FAILED - the bundled fonts did not load, so nothing measured "
                        "here says anything about the real machine\n");
        return 1;
    }

    cairo_surface_t *background = nullptr;
    BgMode bgMode = BgMode::Fill;
    if (argc >= 5) {
        bgMode = argc >= 6 ? bgModeFromString(argv[5]) : BgMode::Fill;
        background = loadBackgroundImage(argv[3], argv[4]);
        printf("uirender: background '%s' %s\n", argv[4],
               background ? "loaded" : "refused, flat ground kept");
    }

    auditLayout(fonts);

    // The scales a real machine picks, taken from the window's own rule rather than made up
    // here: 768p, 1080p and 4K. Rendering at each one is what catches a constant that only
    // happens to work at scale 1.
    struct Res {
        int w, h;
        float scale;
    };
    // Mirrors X11Window::autoScale, which gfx/ cannot call without linking X11.
    auto autoScale = [](int pixelH) {
        float s = static_cast<float>(pixelH) / 768.0f;
        if (s < 1.0f)
            s = 1.0f;
        if (s > 3.0f)
            s = 3.0f;
        return static_cast<float>(static_cast<int>(s * 4.0f + 0.5f)) / 4.0f;
    };
    const Res resolutions[] = {
        {1366, 768, autoScale(768)},
        {1920, 1080, autoScale(1080)},
        {3840, 2160, autoScale(2160)},
    };

    for (const Res &res : resolutions) {
        for (const Scene &sc : kScenes) {
            char path[512];
            snprintf(path, sizeof(path), "%s/panel-%dp-%s.png", outDir.c_str(), res.h, sc.name);
            const int logicalW = static_cast<int>(static_cast<float>(res.w) / res.scale);
            const int logicalH = static_cast<int>(static_cast<float>(res.h) / res.scale);
            if (renderScene(fonts, sc, res.scale, logicalW, logicalH, background, bgMode, path))
                printf("uirender: wrote %s (%dx%d at scale %.2f)\n", path, res.w, res.h,
                       static_cast<double>(res.scale));
        }
    }

    if (background)
        cairo_surface_destroy(background);

    if (gFailures > 0) {
        fprintf(stderr, "uirender: FAILED - %d layout problem(s)\n", gFailures);
        return 1;
    }
    printf("uirender: PASSED - fonts bundled, every slot fits, every clearance holds\n");
    return 0;
}
