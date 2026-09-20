// uirender -- compose the drawing layer to a PNG with no X server running.
//
// This is the cheap half of the test strategy and the reason src/gfx/ is kept free of Xlib: a
// login screen can otherwise only be looked at by rebooting into it, which is a slow way to
// find out that a label is clipped. Everything here uses the same Canvas, the same FontStack
// and the same palette the real window uses, so what it renders is what the screen renders.
//
// Usage:  tools/uirender <resource-dir> <out.png> [background-dir] [background-name] [mode]
//
// Exits non-zero if the bundled fonts did not load. That is deliberate: measuring text against
// a substituted system face proves nothing about whether it fits on the real machine.

#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "gfx/image.h"
#include "gfx/ink.h"
#include "gfx/palette.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace xlogin;

namespace
{

// The worst-case strings this layer has to survive. Real PAM messages, a username at the limit
// the old GTK code enforced, and the longest status text the UI can produce.
const char *const kWorstCase[] = {
    "Authentication failure",
    "Your account has expired; please contact your system administrator",
    "aVeryLongUserNameThatSomebodyWillHaveSoonerOrLater",
    "Permission denied",
    "Starting session...",
};

struct Swatch {
    const char *name;
    uint32_t rgb;
};

const Swatch kSwatches[] = {
    {"kBgColor", pal::kBgColor},
    {"kFaceColor", pal::kFaceColor},
    {"kWellColor", pal::kWellColor},
    {"kGold", pal::kGold},
    {"kTextColor", pal::kTextColor},
    {"kDimColor", pal::kDimColor},
    {"kAccent", pal::kAccent},
    {"kAccentBright", pal::kAccentBright},
    {"kDisabledColor", pal::kDisabledColor},
    {"kWarnColor", pal::kWarnColor},
    {"kErrorColor", pal::kErrorColor},
};

int gOverflows = 0;

// Draw a string and complain if it did not fit the slot it was given. This is the check that
// makes the tool an audit rather than a screenshot.
void drawChecked(Canvas &c, const char *text, float x, float y, float maxW)
{
    const float w = c.stringWidth(text);
    if (w > maxW) {
        fprintf(stderr, "uirender: OVERFLOW %.1f > %.1f for \"%s\"\n", static_cast<double>(w),
                static_cast<double>(maxW), text);
        ++gOverflows;
    }
    c.drawString(c.clipToWidth(text, maxW).c_str(), x, y);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <resource-dir> <out.png> [bg-dir] [bg-name] [fill|fit|center|"
                "stretch|tile]\n",
                argv[0]);
        return 2;
    }

    const std::string resourceDir = argv[1];
    const std::string outPath = argv[2];

    FontStack fonts;
    const bool bundled = fonts.load(resourceDir);

    const int w = 900;
    const int h = 560;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "uirender: could not create the surface\n");
        return 1;
    }
    cairo_t *cr = cairo_create(surface);

    {
        Canvas c(cr, &fonts, static_cast<float>(w), static_cast<float>(h));

        // Ground.
        c.setColor(pal::kBgColor);
        c.fillRect(c.bounds());

        // Optional background image, exercised through exactly the path the window uses.
        if (argc >= 5) {
            const BgMode mode = argc >= 6 ? bgModeFromString(argv[5]) : BgMode::Fill;
            cairo_surface_t *bg = loadBackgroundImage(argv[3], argv[4]);
            if (bg) {
                drawBackground(c, bg, c.bounds(), mode);
                cairo_surface_destroy(bg);
                printf("uirender: background '%s' drawn as %s\n", argv[4], bgModeName(mode));
            } else {
                printf("uirender: background '%s' refused, flat ground kept\n", argv[4]);
            }
        }

        // Title face.
        c.setFont(Font::Title);
        c.setFontSize(22.0f);
        c.setColor(pal::kTextColor);
        drawChecked(c, "XLOGIN", 24.0f, 48.0f, 300.0f);

        c.setColor(pal::kGold, 90);
        c.setPenSize(1.0f);
        c.strokeLine(24.0f, 62.0f, static_cast<float>(w) - 24.0f, 62.0f);

        // Body face at every size the panel will use, with the worst-case strings.
        c.setFont(Font::Body);
        float y = 92.0f;
        for (float size : {9.0f, 10.0f, 11.0f, 12.0f, 15.0f}) {
            c.setFontSize(size);
            c.setColor(pal::kDimColor);
            char label[32];
            snprintf(label, sizeof(label), "%.0fpx", static_cast<double>(size));
            c.drawString(label, 24.0f, y);
            c.setColor(pal::kTextColor);
            drawChecked(c, kWorstCase[1], 70.0f, y, static_cast<float>(w) - 94.0f);
            y += size + 12.0f;
        }

        // Every worst-case string at the size the status line will use.
        c.setFontSize(11.0f);
        y += 10.0f;
        for (const char *s : kWorstCase) {
            c.setColor(pal::kErrorColor);
            drawChecked(c, s, 24.0f, y, static_cast<float>(w) - 48.0f);
            y += 18.0f;
        }

        // The palette, and the ink rule, drawn the way a control draws them.
        y += 16.0f;
        float x = 24.0f;
        for (const Swatch &s : kSwatches) {
            const Rect box(x, y, 64.0f, 28.0f);
            c.setColor(s.rgb);
            c.fillRoundRect(box, 3.0f);
            c.setColor(pal::kDimColor, kOutlineAlphaIdle);
            c.setPenSize(1.0f);
            c.strokeRoundRect(box, 3.0f);
            x += 72.0f;
            if (x + 64.0f > static_cast<float>(w) - 24.0f) {
                x = 24.0f;
                y += 36.0f;
            }
        }

        // inkFor(), all three states, in the idiom from ink.h.
        y += 48.0f;
        const char *const states[] = {"disabled", "off", "on"};
        const bool enabled[] = {false, true, true};
        const bool on[] = {false, false, true};
        x = 24.0f;
        for (int i = 0; i < 3; ++i) {
            const Rect box(x, y, 120.0f, 30.0f);
            c.setColor(pal::kWellColor);
            c.fillRoundRect(box, 3.0f);
            if (enabled[i] && on[i]) {
                c.setColor(pal::kAccent, kOnFillAlpha);
                c.fillRoundRect(box, 3.0f);
            }
            c.setColor(inkFor(enabled[i], on[i]), i == 2 ? kOutlineAlphaHover : kOutlineAlphaIdle);
            c.setPenSize(1.0f);
            c.strokeRoundRect(box, 3.0f);
            c.setFontSize(11.0f);
            c.setColor(enabled[i] ? pal::kTextColor : pal::kDisabledColor);
            c.drawString(states[i], box.x + 10.0f, box.centerY() + 11.0f * 0.36f);
            x += 132.0f;
        }
    }

    cairo_destroy(cr);
    const cairo_status_t st = cairo_surface_write_to_png(surface, outPath.c_str());
    cairo_surface_destroy(surface);

    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "uirender: could not write %s: %s\n", outPath.c_str(),
                cairo_status_to_string(st));
        return 1;
    }

    printf("uirender: wrote %s\n", outPath.c_str());
    if (!bundled) {
        fprintf(stderr, "uirender: FAILED - the bundled fonts did not load, so nothing measured "
                        "here says anything about the real machine\n");
        return 1;
    }
    if (gOverflows > 0) {
        fprintf(stderr, "uirender: FAILED - %d string(s) overflowed their slot\n", gOverflows);
        return 1;
    }
    printf("uirender: PASSED - fonts bundled, no overflow\n");
    return 0;
}
