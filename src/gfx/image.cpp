// See image.h.

#include "image.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>

#include <algorithm>

#include <jpeglib.h>

namespace xlogin
{

namespace
{

//------------------------------------------------------------------------
// Reading a candidate file, with every check applied before a byte is decoded.

// Open `dir` itself. O_NOFOLLOW is on the directory too: the backgrounds directory being a
// symlink somewhere else is not a configuration this supports, and refusing it is cheaper than
// reasoning about where it points.
int openBackgroundDir(const std::string &dir)
{
    if (dir.empty())
        return -1;
    return open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

// True if this is a file we are willing to decode as root. Refusal is deliberately silent for
// the listing path, which checks many files; loadBackgroundImage() warns for the one it was
// asked for.
bool ownershipIsSafe(const struct stat &st)
{
    if (!S_ISREG(st.st_mode))
        return false;
    if (st.st_uid != 0)
        return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        return false;
    if (st.st_size <= 0 || st.st_size > kMaxImageFileBytes)
        return false;
    return true;
}

// Reads the whole of `fd` into `out`. Returns false on short read, read error or overflow.
bool readAll(int fd, off_t size, std::vector<unsigned char> &out)
{
    out.resize(static_cast<size_t>(size));
    size_t got = 0;
    while (got < out.size()) {
        const ssize_t n = read(fd, out.data() + got, out.size() - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            break; // truncated since the fstat
        got += static_cast<size_t>(n);
    }
    out.resize(got);
    return got == static_cast<size_t>(size);
}

enum class Format { Unknown, Png, Jpeg };

// By content, never by extension. PNG's signature is the 8 bytes below; JPEG starts with the
// SOI marker FF D8 FF.
Format sniff(const std::vector<unsigned char> &b)
{
    static const unsigned char kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() >= sizeof(kPng) && memcmp(b.data(), kPng, sizeof(kPng)) == 0)
        return Format::Png;
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return Format::Jpeg;
    return Format::Unknown;
}

//------------------------------------------------------------------------
// PNG, through cairo, from memory rather than from a path -- so the bytes decoded are exactly
// the bytes that were checked, with no second open in between.

struct PngReader {
    const unsigned char *data = nullptr;
    size_t size = 0;
    size_t offset = 0;
};

cairo_status_t pngRead(void *closure, unsigned char *out, unsigned int length)
{
    PngReader *r = static_cast<PngReader *>(closure);
    if (r->offset + length > r->size)
        return CAIRO_STATUS_READ_ERROR;
    memcpy(out, r->data + r->offset, length);
    r->offset += length;
    return CAIRO_STATUS_SUCCESS;
}

// The IHDR chunk is fixed at the start of every PNG: the 8-byte signature, then a 4-byte
// length and the 4-byte type "IHDR", then width and height as big-endian uint32. Reading them
// here is what makes the dimension cap apply BEFORE anything is allocated -- handing a
// 16384x16384 file straight to cairo would have it decode and allocate a gigabyte first and
// only then be refused, which is not a cap, it is a delayed complaint.
bool pngDimensions(const std::vector<unsigned char> &b, long &w, long &h)
{
    if (b.size() < 24 || memcmp(b.data() + 12, "IHDR", 4) != 0)
        return false;
    const unsigned char *p = b.data() + 16;
    w = (static_cast<long>(p[0]) << 24) | (static_cast<long>(p[1]) << 16) |
        (static_cast<long>(p[2]) << 8) | static_cast<long>(p[3]);
    h = (static_cast<long>(p[4]) << 24) | (static_cast<long>(p[5]) << 16) |
        (static_cast<long>(p[6]) << 8) | static_cast<long>(p[7]);
    return true;
}

cairo_surface_t *decodePng(const std::vector<unsigned char> &bytes)
{
    long iw = 0, ih = 0;
    if (!pngDimensions(bytes, iw, ih))
        return nullptr;
    if (iw <= 0 || ih <= 0 || iw > kMaxImageDimension || ih > kMaxImageDimension)
        return nullptr;

    PngReader reader{bytes.data(), bytes.size(), 0};
    cairo_surface_t *s = cairo_image_surface_create_from_png_stream(pngRead, &reader);
    if (!s)
        return nullptr;
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    const int w = cairo_image_surface_get_width(s);
    const int h = cairo_image_surface_get_height(s);
    if (w <= 0 || h <= 0 || w > kMaxImageDimension || h > kMaxImageDimension) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    return s;
}

//------------------------------------------------------------------------
// JPEG, through libjpeg.

// libjpeg's default error_exit calls exit(), which in this process means the login screen
// vanishes because somebody put a truncated file in a directory. Replaced with a longjmp back
// into the decoder, which then cleans up and returns null.
struct JpegError {
    struct jpeg_error_mgr mgr;
    jmp_buf escape;
};

void jpegErrorExit(j_common_ptr cinfo)
{
    JpegError *err = reinterpret_cast<JpegError *>(cinfo->err);
    longjmp(err->escape, 1);
}

// Silence libjpeg's warnings about corrupt data; we report the outcome ourselves and a login
// screen has nowhere useful to put a per-scanline complaint.
void jpegEmitMessage(j_common_ptr, int)
{
}

// NOTE ON setjmp AND C++: everything between the setjmp below and any longjmp into it is plain
// C and POD. The cairo surface is tracked in a raw pointer and released explicitly on the
// failure path, and the caller's std::vector is constructed before this function is entered, so
// no destructor is skipped by the jump.
cairo_surface_t *decodeJpeg(const std::vector<unsigned char> &bytes)
{
    struct jpeg_decompress_struct cinfo;
    JpegError err;
    cairo_surface_t *surface = nullptr;
    unsigned char *row = nullptr;

    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpegErrorExit;
    err.mgr.emit_message = jpegEmitMessage;

    if (setjmp(err.escape)) {
        jpeg_destroy_decompress(&cinfo);
        free(row);
        if (surface)
            cairo_surface_destroy(surface);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    // Plain RGB rather than one of libjpeg-turbo's JCS_EXT_* orders: the pixels are written
    // below as whole uint32 words, which is correct on either endianness, and this also builds
    // against a stock libjpeg that has no JCS_EXT_* at all.
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int w = static_cast<int>(cinfo.output_width);
    const int h = static_cast<int>(cinfo.output_height);
    if (w <= 0 || h <= 0 || w > kMaxImageDimension || h > kMaxImageDimension ||
        cinfo.output_components != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    unsigned char *base = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);

    row = static_cast<unsigned char *>(malloc(static_cast<size_t>(w) * 3));
    if (!row) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        const int y = static_cast<int>(cinfo.output_scanline);
        JSAMPROW rows[1] = {row};
        if (jpeg_read_scanlines(&cinfo, rows, 1) != 1)
            break;

        // RGB24 is the same 32-bit layout as ARGB32 with the alpha byte ignored, so a JPEG --
        // which is always opaque -- needs no premultiplication at all. Written as uint32 words
        // so the byte order is whatever this machine's is.
        uint32_t *dst = reinterpret_cast<uint32_t *>(base + static_cast<size_t>(y) * stride);
        for (int x = 0; x < w; ++x) {
            const uint32_t r = row[x * 3 + 0];
            const uint32_t g = row[x * 3 + 1];
            const uint32_t b = row[x * 3 + 2];
            dst[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }

    free(row);
    row = nullptr;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    // The pixels were written behind cairo's back.
    cairo_surface_mark_dirty(surface);
    return surface;
}

} // namespace

//------------------------------------------------------------------------
BgMode bgModeFromString(const std::string &s)
{
    if (s == "fit")
        return BgMode::Fit;
    if (s == "center" || s == "centre")
        return BgMode::Center;
    if (s == "stretch")
        return BgMode::Stretch;
    if (s == "tile")
        return BgMode::Tile;
    return BgMode::Fill;
}

const char *bgModeName(BgMode mode)
{
    switch (mode) {
        case BgMode::Fit:
            return "fit";
        case BgMode::Center:
            return "center";
        case BgMode::Stretch:
            return "stretch";
        case BgMode::Tile:
            return "tile";
        case BgMode::Fill:
            break;
    }
    return "fill";
}

//------------------------------------------------------------------------
std::vector<std::string> listBackgroundImages(const std::string &dir)
{
    std::vector<std::string> names;

    const int dirFd = openBackgroundDir(dir);
    if (dirFd < 0)
        return names;

    // fdopendir takes ownership of dirFd: closedir closes it, and it must not be closed here.
    DIR *d = fdopendir(dirFd);
    if (!d) {
        ::close(dirFd);
        return names;
    }

    while (const struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;

        const int fd = openat(dirFd, e->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct stat st;
        if (fstat(fd, &st) != 0 || !ownershipIsSafe(st)) {
            ::close(fd);
            continue;
        }

        // Enough to sniff, not the whole file: the listing runs over every entry and there is
        // no reason to read a 40 MB photograph to find out it is a photograph.
        unsigned char head[8];
        const ssize_t n = read(fd, head, sizeof(head));
        ::close(fd);
        if (n < 3)
            continue;

        const std::vector<unsigned char> probe(head, head + n);
        if (sniff(probe) != Format::Unknown)
            names.push_back(e->d_name);
    }

    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

//------------------------------------------------------------------------
cairo_surface_t *loadBackgroundImage(const std::string &dir, const std::string &name)
{
    // A caller may name a file in the directory and nothing else. This is belt and braces --
    // openat with O_NOFOLLOW already refuses to traverse a symlink -- but "../" is not a
    // symlink, and this is the check that stops it.
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") {
        fprintf(stderr, "xlogin: refusing background name '%s': not a plain filename\n",
                name.c_str());
        return nullptr;
    }

    const int dirFd = openBackgroundDir(dir);
    if (dirFd < 0) {
        fprintf(stderr, "xlogin: cannot open the backgrounds directory '%s'\n", dir.c_str());
        return nullptr;
    }

    const int fd = openat(dirFd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(dirFd);
    if (fd < 0) {
        fprintf(stderr, "xlogin: cannot open background '%s'\n", name.c_str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !ownershipIsSafe(st)) {
        ::close(fd);
        fprintf(stderr,
                "xlogin: refusing background '%s': it must be a regular file owned by root, "
                "not group- or world-writable, and under %ld bytes\n",
                name.c_str(), kMaxImageFileBytes);
        return nullptr;
    }

    std::vector<unsigned char> bytes;
    const bool ok = readAll(fd, st.st_size, bytes);
    ::close(fd);
    if (!ok) {
        fprintf(stderr, "xlogin: could not read background '%s'\n", name.c_str());
        return nullptr;
    }

    cairo_surface_t *surface = nullptr;
    switch (sniff(bytes)) {
        case Format::Png:
            surface = decodePng(bytes);
            break;
        case Format::Jpeg:
            surface = decodeJpeg(bytes);
            break;
        case Format::Unknown:
            fprintf(stderr, "xlogin: background '%s' is neither a PNG nor a JPEG\n", name.c_str());
            return nullptr;
    }

    if (!surface) {
        fprintf(stderr, "xlogin: could not decode background '%s'\n", name.c_str());
        return nullptr;
    }
    return surface;
}

//------------------------------------------------------------------------
void drawBackground(Canvas &c, cairo_surface_t *image, const Rect &dest, BgMode mode)
{
    if (!image || dest.w <= 0 || dest.h <= 0)
        return;

    const float iw = static_cast<float>(cairo_image_surface_get_width(image));
    const float ih = static_cast<float>(cairo_image_surface_get_height(image));
    if (iw <= 0 || ih <= 0)
        return;

    cairo_t *cr = c.cr();
    cairo_save(cr);
    cairo_rectangle(cr, dest.x, dest.y, dest.w, dest.h);
    cairo_clip(cr);

    if (mode == BgMode::Tile) {
        cairo_translate(cr, dest.x, dest.y);
        cairo_set_source_surface(cr, image, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_rectangle(cr, 0, 0, dest.w, dest.h);
        cairo_fill(cr);
        cairo_restore(cr);
        return;
    }

    float sx = 1.0f, sy = 1.0f;
    switch (mode) {
        case BgMode::Stretch:
            sx = dest.w / iw;
            sy = dest.h / ih;
            break;
        case BgMode::Fit: // contain: the whole image is visible, with ground showing at the sides
            sx = sy = std::min(dest.w / iw, dest.h / ih);
            break;
        case BgMode::Center: // 1:1, cropped by the clip above if it is larger than the screen
            sx = sy = 1.0f;
            break;
        case BgMode::Fill: // cover: no ground shows, the overflow is cropped
        case BgMode::Tile: // handled above
            sx = sy = std::max(dest.w / iw, dest.h / ih);
            break;
    }

    // Centre whatever the scale produced inside dest.
    const float dw = iw * sx;
    const float dh = ih * sy;
    cairo_translate(cr, dest.x + (dest.w - dw) * 0.5f, dest.y + (dest.h - dh) * 0.5f);
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, image, 0, 0);
    // Without this a scaled-up image samples past its own edge and picks up transparent pixels,
    // which shows as a one-pixel dark seam along the top and left of a Fill background.
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);

    cairo_restore(cr);
}

} // namespace xlogin
