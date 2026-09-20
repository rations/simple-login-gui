/* See cleanup.h. */

#define _GNU_SOURCE
#include "cleanup.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* How long to let a process honour SIGTERM before it is killed. Half a second, carried over
 * from the implementation this replaces, where it was enough for every client that was ever
 * observed to leak. Long enough to be polite, short enough that a logout does not feel hung. */
#define TERM_GRACE_NS (500L * 1000L * 1000L)

/* Walk /proc rather than exec pkill.
 *
 * pkill would mean a fork, an exec, a $PATH lookup from a root process and a runtime
 * dependency on procps -- all three of which this program's rules forbid or would rather not
 * have. Reading /proc is what pkill does anyway.
 *
 * IT READS /proc/<pid>/status AND NOT THE OWNER OF /proc/<pid>. The directory's owner is the
 * EFFECTIVE uid, and the kernel reports it as root:root for any process whose "dumpable"
 * attribute is not 1 (cites: proc_pid(5), "the ownership is made root:root if the process's
 * dumpable attribute is set to a value other than 1") -- which is exactly what a setuid helper
 * left over from a session looks like. Filtering on the directory owner would silently miss
 * those. The status file's Uid line is readable by root whatever the dumpable flag says, and
 * its first field is the REAL uid (cites: proc_pid_status(5), "Real, effective, saved set, and
 * filesystem UIDs").
 */
static int read_real_uid(const char *pid_str, uid_t *out)
{
    char path[64];
    FILE *fp;
    char line[256];
    int found = 0;

    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);
    fp = fopen(path, "re");
    if (!fp)
        return 0; /* the process exited between readdir and here; not an error */

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned long v;
            if (sscanf(line + 4, "%lu", &v) == 1) {
                *out = (uid_t)v;
                found = 1;
            }
            break;
        }
    }

    fclose(fp);
    return found;
}

static int all_digits(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

static void sleep_ns(long ns)
{
    struct timespec ts;
    ts.tv_sec = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* Restart with the remainder nanosleep wrote back. */
    }
}

static int signal_user_processes(uid_t uid, int sig)
{
    DIR *d;
    struct dirent *e;
    const pid_t self = getpid();
    int count = 0;

    d = opendir("/proc");
    if (!d)
        return -1;

    while ((e = readdir(d)) != NULL) {
        uid_t owner;
        long pid;

        if (!all_digits(e->d_name))
            continue;
        if (!read_real_uid(e->d_name, &owner) || owner != uid)
            continue;

        pid = strtol(e->d_name, NULL, 10);
        /* Belt and braces. uid 0 is already refused by the caller, so neither of these can
         * fire -- which is the point of asserting them here rather than assuming it. */
        if (pid <= 1 || (pid_t)pid == self)
            continue;

        if (kill((pid_t)pid, sig) == 0)
            count++;
    }

    closedir(d);
    return count;
}

int cleanup_kill_user_processes(uid_t uid)
{
    int n;

    /* This process is root. A bug that reached here with 0 would SIGKILL every process on the
     * machine, starting with init. */
    if (uid == 0) {
        fprintf(stderr, "xlogin: refusing to purge processes for uid 0\n");
        return -1;
    }

    n = signal_user_processes(uid, SIGTERM);
    if (n < 0) {
        fprintf(stderr, "xlogin: could not read /proc to purge the session: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0)
        return 0;

    sleep_ns(TERM_GRACE_NS);
    (void)signal_user_processes(uid, SIGKILL);
    return n;
}

void cleanup_after_session(const char *user)
{
    struct passwd *pw;

    if (!user || !*user)
        return;

    pw = getpwnam(user);
    if (!pw || pw->pw_uid == 0)
        return;

    (void)cleanup_kill_user_processes(pw->pw_uid);
}
