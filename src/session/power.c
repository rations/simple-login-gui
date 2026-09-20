/* See power.h. */

#define _GNU_SOURCE
#include "power.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_message[256];

/* Where shutdown(8) might be. Probed in order with access(X_OK) at call time. */
static const char *const kShutdownPaths[] = {
    "/usr/sbin/shutdown", "/sbin/shutdown", "/usr/bin/shutdown", "/bin/shutdown", NULL,
};

/* An explicit, minimal environment. shutdown(8) needs nothing from ours, and passing ours
 * would hand a root-privileged program whatever the launcher happened to export. */
static char *const kEnv[] = {
    (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
    NULL,
};

static const char *find_shutdown(void)
{
    int i;
    for (i = 0; kShutdownPaths[i]; i++) {
        if (access(kShutdownPaths[i], X_OK) == 0)
            return kShutdownPaths[i];
    }
    return NULL;
}

/* Fork and exec, and find out whether the exec actually happened.
 *
 * THE PIPE IS THE POINT. exec failing in the child is invisible to the parent otherwise -- the
 * child just exits, asynchronously, and the UI would have already said the machine was
 * shutting down. So the child holds a CLOEXEC pipe: a successful exec closes it and the parent
 * reads EOF, a failed exec writes errno down it. This is the standard technique and it is here
 * because "I clicked Shutdown and nothing happened, with no message" is the exact failure this
 * program must not have.
 */
static int spawn(const char *path, char *const argv[], const char **msg)
{
    int pfd[2];
    pid_t pid;
    int err = 0;
    ssize_t n;
    int status = 0;

    if (pipe2(pfd, O_CLOEXEC) != 0) {
        snprintf(g_message, sizeof(g_message), "Could not create a pipe: %s", strerror(errno));
        *msg = g_message;
        return -1;
    }

    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(g_message, sizeof(g_message), "Fork failed: %s", strerror(errno));
        *msg = g_message;
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);
        /* Default dispositions: SIGPIPE is ignored in the parent and an ignored disposition
         * survives exec. */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        execve(path, argv, kEnv);
        /* Only reached if execve failed. */
        {
            const int e = errno;
            ssize_t w = write(pfd[1], &e, sizeof(e));
            (void)w;
        }
        _exit(127);
    }

    close(pfd[1]);
    /* Blocks only until the child execs or dies, which is microseconds either way -- this is
     * not a wait on the shutdown itself. */
    n = read(pfd[0], &err, sizeof(err));
    close(pfd[0]);

    if (n == (ssize_t)sizeof(err)) {
        /* The child told us why. Reap it so it does not linger as a zombie. */
        (void)waitpid(pid, &status, 0);
        snprintf(g_message, sizeof(g_message), "Could not run %s: %s", path, strerror(err));
        *msg = g_message;
        return -1;
    }

    /* EOF: the exec succeeded. Not reaped here -- the main loop's SIGCHLD handler does that,
     * and waiting would block the event loop for as long as shutdown(8) takes. */
    *msg = "";
    return 0;
}

static int do_shutdown(char *const argv[], const char **msg)
{
    const char *path = find_shutdown();
    if (!path) {
        snprintf(g_message, sizeof(g_message),
                 "No shutdown program found in /usr/sbin, /sbin, /usr/bin or /bin");
        *msg = g_message;
        return -1;
    }
    return spawn(path, argv, msg);
}

int power_shutdown(const char **msg)
{
    /* -h halts, -P makes the halt a power-off, and -P is documented as valid only alongside
     * -h. Read out of `shutdown --help` on the target system, not assumed. */
    char *const argv[] = {(char *)"shutdown", (char *)"-h", (char *)"-P", (char *)"now", NULL};
    return do_shutdown(argv, msg);
}

int power_restart(const char **msg)
{
    char *const argv[] = {(char *)"shutdown", (char *)"-r", (char *)"now", NULL};
    return do_shutdown(argv, msg);
}

/* Switch the VT.
 *
 * The ioctl directly rather than forking chvt(1): it is what chvt does anyway, and doing it
 * here keeps the no-shell, no-$PATH rule intact and drops a runtime dependency on the kbd
 * package. (cites: linux/vt.h:42 VT_ACTIVATE 0x5606, :43 VT_WAITACTIVE 0x5607,
 * :10-11 MIN_NR_CONSOLES 1 / MAX_NR_CONSOLES 63.)
 *
 * VT_WAITACTIVE IS DELIBERATELY NOT CALLED, although chvt calls it. It blocks until the target
 * VT is actually active, and this program's rules forbid a blocking call in the event loop for
 * a good reason: if the switch does not complete -- and whether it completes at all under
 * `-seat seat0 -keeptty` is the one thing about this feature that cannot be verified from a
 * development machine -- then waiting for it hangs the login screen with no window and no way
 * back. Nothing here needs to know WHEN the switch finished, only that it was requested. So it
 * is requested, and the screen stays alive either way.
 */
int power_switch_console(int vt, const char **msg)
{
    int fd;

    if (vt < MIN_NR_CONSOLES || vt > MAX_NR_CONSOLES) {
        snprintf(g_message, sizeof(g_message), "VT %d is out of range (%d-%d)", vt, MIN_NR_CONSOLES,
                 MAX_NR_CONSOLES);
        *msg = g_message;
        return -1;
    }

    /* /dev/tty0 is the current virtual console, whichever that is, so this works without
     * knowing or caring which VT the X server took. O_NOCTTY because this must not become
     * the controlling terminal of a process that already has one. */
    fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(g_message, sizeof(g_message), "Could not open /dev/tty0: %s", strerror(errno));
        *msg = g_message;
        return -1;
    }

    if (ioctl(fd, VT_ACTIVATE, vt) != 0) {
        snprintf(g_message, sizeof(g_message), "Could not switch to VT %d: %s", vt,
                 strerror(errno));
        *msg = g_message;
        close(fd);
        return -1;
    }

    close(fd);
    *msg = "";
    return 0;
}

/* Is a getty configured on this VT?
 *
 * A plain scan of /etc/inittab for a respawn line mentioning ttyN. Not a parser: it is used to
 * WARN, never to refuse, so a false negative costs a warning and a false positive costs
 * nothing. Switching to a VT with nothing on it gives a black screen with a cursor, which
 * somebody who has just left a login screen will read as a crash -- that is the whole reason
 * this function exists.
 */
int power_vt_has_getty(int vt)
{
    FILE *fp;
    char line[512];
    char needle[32];
    int found = 0;

    if (vt < MIN_NR_CONSOLES || vt > MAX_NR_CONSOLES)
        return 0;

    fp = fopen("/etc/inittab", "re");
    if (!fp)
        return -1;

    snprintf(needle, sizeof(needle), "tty%d", vt);

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#')
            continue;
        if (!strstr(p, "getty"))
            continue;
        /* "tty2" must not match "tty20". */
        {
            const char *hit = strstr(p, needle);
            while (hit) {
                const char after = hit[strlen(needle)];
                if (after < '0' || after > '9') {
                    found = 1;
                    break;
                }
                hit = strstr(hit + 1, needle);
            }
        }
        if (found)
            break;
    }

    fclose(fp);
    return found;
}
