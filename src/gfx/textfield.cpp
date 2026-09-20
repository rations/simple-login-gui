// See textfield.h.

#include "textfield.h"

#include "../geometry.h"
#include "ink.h"
#include "palette.h"

#include <cstring>
#include <string>

namespace xlogin
{

namespace
{

// Is this byte the continuation of a multi-byte UTF-8 sequence? Every byte of one has its top
// two bits set to 10, and no lead byte or ASCII byte does, which is the property that makes it
// possible to walk a UTF-8 string backwards at all.
inline bool isContinuation(unsigned char b)
{
    return (b & 0xC0) == 0x80;
}

// Reject anything that is not text. C0 controls and DEL arrive as one-byte "text" from the
// input method for Return, Tab, Escape and the control chords, and none of them belongs in a
// username or a password. Everything at 0x80 and above is left alone: it is UTF-8 that the
// input method has already composed, and second-guessing it here is how an accented password
// stops working.
inline bool isTextByte(unsigned char b)
{
    return b >= 0x20 && b != 0x7F;
}

} // namespace

//------------------------------------------------------------------------
TextField::~TextField()
{
    explicit_bzero(mBuf, sizeof(mBuf));
}

//------------------------------------------------------------------------
void TextField::configure(const Rect &r, const std::string &placeholder, bool secret)
{
    mRect = r;
    mPlaceholder = placeholder;
    mSecret = secret;
}

void TextField::setFocused(bool f)
{
    mFocused = f;
}

void TextField::setEnabled(bool e)
{
    mEnabled = e;
    if (!e)
        mHovered = false;
}

//------------------------------------------------------------------------
void TextField::clear()
{
    // The whole buffer, not mLen bytes of it: a short password typed after a long one would
    // otherwise leave the tail of the long one in memory.
    explicit_bzero(mBuf, sizeof(mBuf));
    mLen = 0;
    mCaret = 0;
}

//------------------------------------------------------------------------
Rect TextField::textSlot() const
{
    return Rect::fromLTRB(mRect.left() + geo::kFieldPadX, mRect.top(),
                          mRect.right() - geo::kFieldPadX, mRect.bottom());
}

size_t TextField::charsBeforeCaret() const
{
    size_t n = 0;
    for (size_t i = 0; i < mCaret; ++i) {
        if (!isContinuation(static_cast<unsigned char>(mBuf[i])))
            ++n;
    }
    return n;
}

//------------------------------------------------------------------------
void TextField::insert(const char *utf8, int len)
{
    if (!utf8 || len <= 0)
        return;

    // Copy only the bytes that are text, so a control byte in the middle of a sequence cannot
    // be smuggled in. Sized from the buffer, not from len, because len is attacker-adjacent in
    // the sense that matters: it comes from whoever is at the keyboard.
    char accepted[64];
    size_t n = 0;
    for (int i = 0; i < len && n < sizeof(accepted); ++i) {
        if (isTextByte(static_cast<unsigned char>(utf8[i])))
            accepted[n++] = utf8[i];
    }
    if (n == 0)
        return;

    // Refuse rather than truncate. Truncating a password silently gives the user a login that
    // fails with no explanation, and truncating mid-sequence would leave half a character.
    if (mLen + n >= kCapacity)
        return;

    memmove(mBuf + mCaret + n, mBuf + mCaret, mLen - mCaret);
    memcpy(mBuf + mCaret, accepted, n);
    mLen += n;
    mCaret += n;
    mBuf[mLen] = '\0';

    explicit_bzero(accepted, sizeof(accepted));
}

void TextField::deleteBack()
{
    if (mCaret == 0)
        return;

    size_t start = mCaret - 1;
    while (start > 0 && isContinuation(static_cast<unsigned char>(mBuf[start])))
        --start;

    const size_t n = mCaret - start;
    memmove(mBuf + start, mBuf + mCaret, mLen - mCaret);
    mLen -= n;
    mCaret = start;
    // Zero the tail the move vacated, or the deleted bytes stay in the buffer past mLen.
    explicit_bzero(mBuf + mLen, kCapacity - mLen);
}

void TextField::deleteForward()
{
    if (mCaret >= mLen)
        return;

    size_t end = mCaret + 1;
    while (end < mLen && isContinuation(static_cast<unsigned char>(mBuf[end])))
        ++end;

    const size_t n = end - mCaret;
    memmove(mBuf + mCaret, mBuf + end, mLen - end);
    mLen -= n;
    explicit_bzero(mBuf + mLen, kCapacity - mLen);
}

void TextField::moveLeft()
{
    if (mCaret == 0)
        return;
    --mCaret;
    while (mCaret > 0 && isContinuation(static_cast<unsigned char>(mBuf[mCaret])))
        --mCaret;
}

void TextField::moveRight()
{
    if (mCaret >= mLen)
        return;
    ++mCaret;
    while (mCaret < mLen && isContinuation(static_cast<unsigned char>(mBuf[mCaret])))
        ++mCaret;
}

//------------------------------------------------------------------------
bool TextField::handleKey(Key k, const char *utf8, int len)
{
    if (!mEnabled || !mFocused)
        return false;

    switch (k) {
        case Key::Backspace:
            deleteBack();
            return true;
        case Key::Delete:
            deleteForward();
            return true;
        case Key::Left:
            moveLeft();
            return true;
        case Key::Right:
            moveRight();
            return true;
        case Key::Home:
            mCaret = 0;
            return true;
        case Key::End:
            mCaret = mLen;
            return true;

        // Not ours. The panel moves the focus, submits, or clears -- a field cannot know which
        // of those a Tab or a Return means.
        case Key::Tab:
        case Key::BackTab:
        case Key::Enter:
        case Key::Escape:
        case Key::Up:
        case Key::Down:
            return false;

        case Key::Plain:
            break;
    }

    // An ordinary character, including anything a dead-key or Compose sequence composed. Note
    // that Shift is NOT filtered out here: a sibling project records the bug where refusing any
    // keystroke carrying a modifier made capital letters untypeable, and on a login screen that
    // would be untypeable passwords.
    if (len > 0) {
        insert(utf8, len);
        return true;
    }
    return false;
}

//------------------------------------------------------------------------
bool TextField::handleClick(float x, float y)
{
    if (!mEnabled || !mRect.contains(x, y))
        return false;
    mFocused = true;
    mCaret = mLen;
    return true;
}

//------------------------------------------------------------------------
float TextField::contentWidth(const Canvas &c) const
{
    if (mSecret) {
        // Dots, counted. The plaintext is not measured, and is not passed to cairo at all.
        size_t chars = 0;
        for (size_t i = 0; i < mLen; ++i) {
            if (!isContinuation(static_cast<unsigned char>(mBuf[i])))
                ++chars;
        }
        return static_cast<float>(chars) * geo::kDotPitch;
    }
    return c.stringWidth(mBuf);
}

//------------------------------------------------------------------------
// How far to slide the contents left so the caret stays on screen.
//
// Zero until the caret would fall outside the slot, then just enough to bring it to the right
// edge. No centring and no hysteresis: this is a one-line field that is nearly always shorter
// than its slot, and a cleverer rule would only be visible in the case that does not happen.
float TextField::scrollOffset(const Canvas &c) const
{
    const float slotW = textSlot().w;

    float caretX;
    if (mSecret) {
        caretX = static_cast<float>(charsBeforeCaret()) * geo::kDotPitch;
    } else {
        const std::string before(mBuf, mCaret);
        caretX = c.stringWidth(before.c_str());
    }

    if (caretX <= slotW - geo::kCaretW)
        return 0.0f;
    return caretX - (slotW - geo::kCaretW);
}

//------------------------------------------------------------------------
void TextField::draw(Canvas &c) const
{
    // The well, then the outline. Same three steps as every other control in this family: fill,
    // optional accent wash, 1px outline whose ALPHA is the whole focus/hover treatment.
    c.setColor(pal::kWellColor);
    c.fillRoundRect(mRect, geo::kFieldRadius);

    c.setColor(inkFor(mEnabled, mFocused),
               (mFocused || mHovered) ? kOutlineAlphaHover : kOutlineAlphaIdle);
    c.setPenSize(1.0f);
    c.strokeRoundRect(mRect, geo::kFieldRadius);

    const Rect slot = textSlot();
    const float baseline = geo::fieldTextBaseline(mRect.y);

    // Clip to the slot so a long value scrolls under the padding instead of over the outline.
    c.pushClip(slot);

    const float dx = scrollOffset(c);

    if (mLen == 0 && !mPlaceholder.empty()) {
        c.setFont(Font::Body);
        c.setFontSize(geo::kFieldTextSize);
        c.setColor(pal::kDisabledColor);
        c.drawString(mPlaceholder.c_str(), slot.x, baseline);
    } else if (mSecret) {
        // One dot per CHARACTER, drawn as a circle. Never a glyph, and never the plaintext.
        const size_t chars = charsBeforeCaret() + [&] {
            size_t after = 0;
            for (size_t i = mCaret; i < mLen; ++i) {
                if (!isContinuation(static_cast<unsigned char>(mBuf[i])))
                    ++after;
            }
            return after;
        }();
        c.setColor(mEnabled ? pal::kTextColor : pal::kDisabledColor);
        const float cy = mRect.centerY();
        for (size_t i = 0; i < chars; ++i) {
            const float cx =
                slot.x - dx + geo::kDotPitch * 0.5f + geo::kDotPitch * static_cast<float>(i);
            c.fillEllipse(cx, cy, geo::kDotRadius, geo::kDotRadius);
        }
    } else {
        c.setFont(Font::Body);
        c.setFontSize(geo::kFieldTextSize);
        c.setColor(mEnabled ? pal::kTextColor : pal::kDisabledColor);
        c.drawString(mBuf, slot.x - dx, baseline);
    }

    // The caret. No blink: a blink needs a timer, a timer needs a repaint every half second,
    // and a login screen that repaints forever is a login screen that never idles.
    if (mFocused && mEnabled) {
        float caretX;
        if (mSecret) {
            caretX = geo::kDotPitch * static_cast<float>(charsBeforeCaret());
        } else {
            const std::string before(mBuf, mCaret);
            caretX = c.stringWidth(before.c_str());
        }
        const float x = slot.x - dx + caretX;
        const float half = geo::kFieldTextSize * 0.62f;
        c.setColor(pal::kAccentBright);
        c.setPenSize(geo::kCaretW);
        c.strokeLine(x, mRect.centerY() - half, x, mRect.centerY() + half);
    }

    c.popClip();
}

} // namespace xlogin
