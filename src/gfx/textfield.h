// TextField -- one line of editable UTF-8 text, and, for the password, the one place in this
// program where the plaintext lives.
//
// Written for this project rather than ported. The sibling projects have a text entry, and its
// shape is the right one, but its input handling is not adequate here: it reads keystrokes with
// XLookupString and filters them to 0x20-0x7E, it has no masking, and there is no XOpenIM or
// Xutf8LookupString anywhere in that tree. That is the right trade for typing a preset name. It
// is the wrong one for a login screen, where a user whose password contains an accented
// character would simply be unable to log in. So the input method lives in the window
// (platform/x11window.cpp), and this field takes the UTF-8 that came out of it.
//
// THE PASSWORD RULES, which are why this class owns a raw array rather than a std::string:
//
//   * A fixed char[kCapacity]. std::string reallocates as it grows, and the old allocation
//     keeps the bytes it held -- freed, unzeroed, and still in the heap for anything that can
//     read this process's memory or a core file of it.
//   * explicit_bzero, never memset. The compiler is entitled to delete a memset whose result is
//     never read, which is exactly the shape of every erase in this file.
//     (cites: string.h:466 for the declaration.)
//   * Erased on submit, on clear, on losing the contents for any reason, and in the destructor.
//   * A MASKED FIELD NEVER HANDS ITS TEXT TO CAIRO. The dots are drawn as circles at a fixed
//     pitch and the caret is placed by counting characters, so the plaintext is never measured,
//     never shaped, never in a glyph cache and never in a string cairo owns. That is also why
//     the mask is not a bullet glyph: a glyph would have to be looked up in the body face, and
//     drawing one dot per character means one dot per character even in a face that has no
//     U+2022 at all.
//   * No mouse selection and no clipboard. A login field does not need paste, and touching the
//     PRIMARY selection from a root process before anybody has authenticated is a liability for
//     a convenience nobody asked for.
//
// Everything is indexed in BYTES internally and moves in CHARACTERS externally: Backspace over
// an e-acute deletes the whole two-byte sequence, and the caret never lands inside one.

#pragma once

#include "canvas.h"
#include "keys.h"

#include <cstddef>
#include <string>

namespace xlogin
{

class TextField
{
public:
    // 512 bytes, matching the buffer PAM's own conversation is given elsewhere in this program
    // and comfortably over any password a PAM module will accept -- pam_unix truncates at 8 for
    // traditional crypt and at 256 for the modern hashes. Fixed, so it can be erased.
    static constexpr size_t kCapacity = 512;

    TextField() = default;
    ~TextField();

    TextField(const TextField &) = delete;
    TextField &operator=(const TextField &) = delete;

    // `secret` makes it a password field: masked on screen, and never measured as text.
    void configure(const Rect &r, const std::string &placeholder, bool secret);

    //--- state ---------------------------------------------------------
    const Rect &rect() const
    {
        return mRect;
    }
    bool secret() const
    {
        return mSecret;
    }
    bool empty() const
    {
        return mLen == 0;
    }
    // The bytes, NUL-terminated. For a password field this is the plaintext: the caller must
    // not copy it anywhere that is not erased, which in this program means it goes straight
    // into the one strdup PAM itself frees and nowhere else.
    const char *text() const
    {
        return mBuf;
    }
    size_t length() const
    {
        return mLen;
    }

    void setFocused(bool f);
    bool focused() const
    {
        return mFocused;
    }
    void setEnabled(bool e);
    bool enabled() const
    {
        return mEnabled;
    }
    void setHovered(bool h)
    {
        mHovered = h;
    }

    // Erase the contents. Zeroes the WHOLE buffer, not just the used part -- a shorter password
    // typed after a longer one would otherwise leave the tail of the first one behind.
    void clear();

    //--- input ---------------------------------------------------------
    // One keystroke. `utf8`/`len` is what the input method produced, which may be empty, and
    // may be several bytes or even several characters after a Compose sequence. Returns true if
    // the field consumed it; Tab, Enter and Escape are returned unconsumed so the panel can act
    // on them.
    bool handleKey(Key k, const char *utf8, int len);

    // A click inside the field focuses it. The caret goes to the END rather than to the click
    // position: placing it by x would mean measuring the text, and for a password field that
    // means measuring the plaintext. Not worth it for a field that is at most a line long.
    bool handleClick(float x, float y);

    //--- drawing -------------------------------------------------------
    void draw(Canvas &c) const;

    // How wide the contents are drawn, in logical units. Exposed for the layout audit, which
    // needs to know whether a full field scrolls rather than overruns. For a masked field this
    // is a count of dots and touches no text.
    float contentWidth(const Canvas &c) const;

private:
    // The slot text is drawn in: the rect inset by kFieldPadX on both sides.
    Rect textSlot() const;
    // How far the contents are scrolled left so the caret stays visible, in logical units.
    float scrollOffset(const Canvas &c) const;
    size_t charsBeforeCaret() const;

    void insert(const char *utf8, int len);
    void deleteBack();
    void deleteForward();
    void moveLeft();
    void moveRight();

    Rect mRect;
    std::string mPlaceholder;
    bool mSecret = false;

    // The contents. Always NUL-terminated at mLen. Never a std::string: see the header comment.
    char mBuf[kCapacity] = {};
    size_t mLen = 0;
    size_t mCaret = 0; // byte index, always on a character boundary

    bool mFocused = false;
    bool mEnabled = true;
    bool mHovered = false;
};

} // namespace xlogin
