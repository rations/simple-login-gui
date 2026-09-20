/* See config.h. */

#define _GNU_SOURCE
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef XLOGIN_CONFIG_PATH
#define XLOGIN_CONFIG_PATH "/etc/xlogin.conf"
#endif

/* Bounds. The config is a handful of KEY=value lines; anything bigger is not a config file,
 * and this process is root and pre-authentication, so "not a config file" is refused rather
 * than parsed harder. */
#define CFG_MAX_BYTES (64 * 1024)
#define CFG_MAX_LINE 1024

const char *config_path(void)
{
    return XLOGIN_CONFIG_PATH;
}

void config_defaults(xlogin_config *c)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->background[0] = '\0';
    snprintf(c->bg_mode, sizeof(c->bg_mode), "fill");
    /* tty2, where the first getty respawns on a stock inittab. */
    c->console_vt = 2;
}

/* Strip one layer of matching quotes and trailing whitespace, in place. The same shape as the
 * launcher's shell would see, and the same shape load_locale_env() uses on /etc/default/locale
 * -- one parser idiom in this program, not two. */
static char *clean_value(char *val)
{
    size_t vlen = strlen(val);
    while (vlen && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' || val[vlen - 1] == ' ' ||
                    val[vlen - 1] == '\t'))
        val[--vlen] = '\0';
    if (vlen >= 2 && (val[0] == '"' || val[0] == '\'') && val[vlen - 1] == val[0]) {
        val[vlen - 1] = '\0';
        val++;
    }
    return val;
}

void config_load(xlogin_config *c)
{
    FILE *fp;
    char line[CFG_MAX_LINE];
    long total = 0;

    if (!c)
        return;
    config_defaults(c);

    fp = fopen(XLOGIN_CONFIG_PATH, "re");
    if (!fp)
        return; /* no file is not an error: the defaults are the answer */

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *eq;
        char *key;
        char *val;

        total += (long)strlen(line);
        if (total > CFG_MAX_BYTES) {
            fprintf(stderr, "xlogin: %s is over %d bytes; ignoring the rest\n", XLOGIN_CONFIG_PATH,
                    CFG_MAX_BYTES);
            break;
        }

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        if (strncmp(p, "export ", 7) == 0)
            p += 7;

        eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = p;
        val = clean_value(eq + 1);

        if (strcmp(key, "XLOGIN_BACKGROUND") == 0) {
            snprintf(c->background, sizeof(c->background), "%s", val);
        } else if (strcmp(key, "XLOGIN_BG_MODE") == 0) {
            snprintf(c->bg_mode, sizeof(c->bg_mode), "%s", val);
        } else if (strcmp(key, "XLOGIN_CONSOLE_VT") == 0) {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* MIN_NR_CONSOLES..MAX_NR_CONSOLES (cites: linux/vt.h:10-11). Out of range is
             * ignored rather than clamped: a typo should leave the default, not silently send
             * somebody to a VT they did not mean. */
            if (end && end != val && v >= 1 && v <= 63)
                c->console_vt = (int)v;
            else
                fprintf(stderr,
                        "xlogin: XLOGIN_CONSOLE_VT=%s is not a VT number 1-63; "
                        "using %d\n",
                        val, c->console_vt);
        }
    }

    fclose(fp);
}

/* Can this value go inside single quotes in a file that /bin/sh will source?
 *
 * Inside single quotes a POSIX shell treats every character literally and there is no escape
 * -- which is exactly why this is the quoting to use, and exactly why a single quote in the
 * value cannot be accommodated. Control characters are refused too: a newline would end the
 * line and turn the rest of the value into a command. */
static int safely_quotable(const char *v)
{
    const unsigned char *p = (const unsigned char *)v;
    if (!v)
        return 0;
    for (; *p; p++) {
        if (*p == '\'')
            return 0;
        if (*p < 0x20 || *p == 0x7F)
            return 0;
    }
    return 1;
}

static int key_of_line(const char *line, const char *key)
{
    const char *p = line;
    size_t klen = strlen(key);

    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "export ", 7) == 0)
        p += 7;
    if (strncmp(p, key, klen) != 0)
        return 0;
    p += klen;
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == '=';
}

int config_set(const char *key, const char *value)
{
    char tmp_path[512];
    char line[CFG_MAX_LINE];
    FILE *in;
    FILE *out = NULL;
    int fd = -1;
    int replaced = 0;
    int dirfd = -1;
    int rc = -2;

    if (!key || !value)
        return -1;
    if (!safely_quotable(value)) {
        fprintf(stderr,
                "xlogin: refusing to write %s: the value cannot be safely quoted for a "
                "file the launcher sources as root\n",
                key);
        return -1;
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmpXXXXXX", XLOGIN_CONFIG_PATH);
    fd = mkstemp(tmp_path);
    if (fd < 0) {
        fprintf(stderr, "xlogin: could not create a temporary file beside %s: %s\n",
                XLOGIN_CONFIG_PATH, strerror(errno));
        return -2;
    }
    /* mkstemp gives 0600. The launcher reads this as root, but it is not a secret and a
     * root-only config that somebody cannot read from tty2 is a config they cannot fix. */
    if (fchmod(fd, 0644) != 0)
        fprintf(stderr, "xlogin: could not set the mode on %s: %s\n", tmp_path, strerror(errno));

    out = fdopen(fd, "w");
    if (!out) {
        fprintf(stderr, "xlogin: could not open %s for writing: %s\n", tmp_path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -2;
    }
    fd = -1; /* owned by `out` now */

    in = fopen(XLOGIN_CONFIG_PATH, "re");
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (!replaced && key_of_line(line, key)) {
                /* Replace this line, keeping everything around it. */
                if (fprintf(out, "%s='%s'\n", key, value) < 0)
                    goto write_failed;
                replaced = 1;
            } else if (fputs(line, out) == EOF) {
                goto write_failed;
            }
        }
        fclose(in);
        in = NULL;
    } else {
        /* First write. Leave a note for whoever opens this file with an editor, since the
         * launcher sources it and that is not obvious from the contents. */
        if (fprintf(out, "# xlogin settings. This file is sourced by /bin/sh, so every value\n"
                         "# must be a plain KEY='value' line.\n") < 0)
            goto write_failed;
    }

    if (!replaced) {
        if (fprintf(out, "%s='%s'\n", key, value) < 0)
            goto write_failed;
    }

    /* Flush stdio into the file, then the file onto the disk, before the rename. Without the
     * fsync the rename can land ahead of the data and a power cut leaves an empty config. */
    if (fflush(out) != 0)
        goto write_failed;
    if (fsync(fileno(out)) != 0)
        goto write_failed;
    if (fclose(out) != 0) {
        out = NULL;
        goto write_failed;
    }
    out = NULL;

    if (rename(tmp_path, XLOGIN_CONFIG_PATH) != 0) {
        fprintf(stderr, "xlogin: could not replace %s: %s\n", XLOGIN_CONFIG_PATH, strerror(errno));
        unlink(tmp_path);
        return -2;
    }

    /* And the directory entry itself, so the rename survives too. */
    {
        char dir[512];
        char *slash;
        snprintf(dir, sizeof(dir), "%s", XLOGIN_CONFIG_PATH);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            dirfd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dirfd >= 0) {
                if (fsync(dirfd) != 0)
                    fprintf(stderr, "xlogin: could not fsync %s: %s\n", dir, strerror(errno));
                close(dirfd);
            }
        }
    }
    return 0;

write_failed:
    fprintf(stderr, "xlogin: could not write %s: %s\n", tmp_path, strerror(errno));
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    unlink(tmp_path);
    return rc;
}
