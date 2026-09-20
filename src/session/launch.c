/* See launch.h. */

#define _GNU_SOURCE
#include "launch.h"

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_message[256];

/* Load the system locale into the (already-cleared) child environment.
 *
 * We clearenv() and rebuild a minimal session environment, so unless we set LANG/LC_* here the
 * session inherits no locale and glibc falls back to C/POSIX, whose encoding is ASCII -- every
 * UTF-8 character (em-dashes, box-drawing, accented letters) then renders as garbage in
 * terminals and TUIs. A normal getty/login gets these from PAM's pam_env via
 * /etc/default/locale; we read that file (then /etc/environment) ourselves. Falls back to
 * C.UTF-8 so the session is at least UTF-8 capable even when no system locale is configured.
 *
 * Ported unchanged. Runs in the CHILD, after the privilege drop. */
static void load_locale_env(void)
{
    static const char *const keys[] = {
        "LANG",    "LANGUAGE",   "LC_ALL",       "LC_CTYPE",       "LC_NUMERIC",
        "LC_TIME", "LC_COLLATE", "LC_MONETARY",  "LC_MESSAGES",    "LC_PAPER",
        "LC_NAME", "LC_ADDRESS", "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION",
        NULL};
    static const char *const files[] = {"/etc/default/locale", "/etc/environment", NULL};

    for (int f = 0; files[f]; f++) {
        FILE *fp = fopen(files[f], "re");
        if (!fp)
            continue;
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '#' || *p == '\n' || *p == '\0')
                continue;
            if (strncmp(p, "export ", 7) == 0)
                p += 7; /* tolerate "export FOO=" */

            char *eq = strchr(p, '=');
            if (!eq)
                continue;
            *eq = '\0';
            char *key = p, *val = eq + 1;

            size_t vlen = strlen(val); /* trim trailing space/EOL */
            while (vlen && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
                val[--vlen] = '\0';
            if (vlen >= 2 && (val[0] == '"' || val[0] == '\'') && val[vlen - 1] == val[0]) {
                /* strip matching quotes */
                val[vlen - 1] = '\0';
                val++;
            }

            for (int k = 0; keys[k]; k++) {
                if (strcmp(key, keys[k]) == 0) {
                    setenv(key, val, 1);
                    break;
                }
            }
        }
        fclose(fp);
    }

    /* Nothing configured? Still guarantee a UTF-8-capable session. */
    if (!getenv("LANG") && !getenv("LC_ALL") && !getenv("LC_CTYPE"))
        setenv("LANG", "C.UTF-8", 1);
}

static launch_result fail(const char *why)
{
    launch_result r;
    r.ok = 0;
    r.pid = -1;
    snprintf(g_message, sizeof(g_message), "%s", why);
    r.message = g_message;
    return r;
}

launch_result launch_session(const char *user)
{
    struct passwd *pw;
    char runtime_dir[64];
    pid_t pid;

    if (!user || !*user)
        return fail("No user");

    pw = getpwnam(user);
    if (!pw)
        return fail("User not found");

    /* Ensure /run/user exists -- not created by sysvinit on seatd-only systems. */
    if (mkdir("/run/user", 0755) == -1 && errno != EEXIST)
        return fail("Failed to create /run/user");

    /* Create /run/user/<uid> with the right ownership. */
    snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%u", (unsigned)pw->pw_uid);
    if (mkdir(runtime_dir, 0700) == -1 && errno != EEXIST)
        return fail("Failed to create runtime dir");
    /* Non-fatal, and warned about rather than ignored: a runtime directory the user cannot
     * write is a session with no D-Bus socket, not a session that will not start. */
    if (chown(runtime_dir, pw->pw_uid, pw->pw_gid) != 0)
        fprintf(stderr, "xlogin: could not chown %s: %s\n", runtime_dir, strerror(errno));
    if (chmod(runtime_dir, 0700) != 0)
        fprintf(stderr, "xlogin: could not chmod %s: %s\n", runtime_dir, strerror(errno));

    /* Flush before forking, or anything still in a stdio buffer is written twice. */
    fflush(NULL);

    pid = fork();
    if (pid < 0)
        return fail("Fork failed");

    if (pid == 0) {
        /* -- child: become the user and exec their session -------------------------------- */

        /* Reset signal handlers to defaults. The parent's SIGCHLD handler in particular must
         * not survive into the session. */
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        /* Close inherited file descriptors above stderr. The X connection is one of them, and
         * so is the self-pipe the parent watches -- neither belongs to the user. */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0)
            maxfd = 1024;
        for (long fd = 3; fd < maxfd; fd++)
            close((int)fd);

        /* Sanitise the environment BEFORE dropping privileges, so nothing inherited from the
         * launcher reaches a process running as the user. */
        clearenv();

        /* Drop privileges -- the order is the whole point; see launch.h. */
        if (setgid(pw->pw_gid) != 0)
            _exit(1);
        if (initgroups(user, pw->pw_gid) != 0)
            _exit(1);
        if (setuid(pw->pw_uid) != 0)
            _exit(1);

        /* Build a minimal, clean session environment. */
        setenv("USER", user, 1);
        setenv("LOGNAME", user, 1);
        setenv("HOME", pw->pw_dir, 1);
        setenv("SHELL", pw->pw_shell, 1);
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
        setenv("DISPLAY", ":0", 1);
        setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
        setenv("XDG_SEAT", "seat0", 1);
        /* No XAUTHORITY -- the X server was started with -ac, so there is no cookie to point
         * at, and a stale XAUTHORITY pointing at a file the user cannot read is worse than
         * none. Redundant after clearenv() and kept for exactly that reason: it says that the
         * absence is deliberate. */
        unsetenv("XAUTHORITY");

        /* Restore the locale the cleared environment dropped, so the session is UTF-8. */
        load_locale_env();

        if (chdir(pw->pw_dir) != 0) {
            if (chdir("/") != 0) {
                /* Nowhere to go. Carrying on is still better than refusing to log the user
                 * in over a working directory. */
            }
        }

        /* Launch the session: ~/.xinitrc -> system xinitrc -> common WMs -> xterm.
         *
         * This is the one deliberate exception to the no-shell rule, because a .xinitrc IS a
         * shell script -- and note where it happens: after the privilege drop, as the target
         * user, never as root. */
        char xinitrc[PATH_MAX];
        snprintf(xinitrc, sizeof(xinitrc), "%s/.xinitrc", pw->pw_dir);

        if (access(xinitrc, F_OK) == 0) {
            execl("/bin/sh", "sh", "--", xinitrc, (char *)NULL);
        } else if (access("/etc/X11/xinit/xinitrc", F_OK) == 0) {
            execl("/bin/sh", "sh", "--", "/etc/X11/xinit/xinitrc", (char *)NULL);
        } else {
            /* Last resort: try common window managers, then a terminal.
             *
             * execlp searches $PATH, which the no-shell/no-$PATH rule forbids -- in the ROOT
             * process. This is not that: privileges were dropped above, this is the user's
             * own process, and $PATH is the fixed "/usr/local/bin:/usr/bin:/bin" set four
             * lines up rather than anything inherited. The same exception, and the same
             * reason, as the /bin/sh exec above it. */
            static const char *const wms[] = {"jwm",          "openbox-session", "startxfce4",
                                              "mate-session", "xterm",           NULL};
            for (int i = 0; wms[i]; i++) {
                execlp(wms[i], wms[i], (char *)NULL);
                /* execlp returns only on failure -- try the next one. */
            }
        }
        _exit(127);
    }

    /* -- parent ------------------------------------------------------------------------- */
    {
        launch_result r;
        r.ok = 1;
        r.pid = pid;
        r.message = "";
        return r;
    }
}
