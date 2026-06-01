#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <security/pam_appl.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define MAX_USERNAME 256

static GtkWidget *username_entry;
static GtkWidget *password_entry;
static GtkWidget *status_label;
static GtkWidget *login_button;
static pam_handle_t *pamh;

static void update_status(const char *text, gboolean is_error) {
    gtk_label_set_text(GTK_LABEL(status_label), text);
    gtk_widget_set_name(status_label, is_error ? "error-label" : "status-label");
}

static void set_ui_sensitive(gboolean sensitive) {
    gtk_widget_set_sensitive(username_entry, sensitive);
    gtk_widget_set_sensitive(password_entry, sensitive);
    gtk_widget_set_sensitive(login_button, sensitive);
}

static int pam_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata_ptr) {
    (void)appdata_ptr;

    *resp = calloc(num_msg, sizeof(struct pam_response));
    if (!*resp) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; i++) {
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
                (*resp)[i].resp = strdup(gtk_entry_get_text(GTK_ENTRY(password_entry)));
                break;
            case PAM_PROMPT_ECHO_ON:
                (*resp)[i].resp = strdup(gtk_entry_get_text(GTK_ENTRY(username_entry)));
                break;
            case PAM_ERROR_MSG:
                g_warning("PAM error: %s", msg[i]->msg);
                break;
            case PAM_TEXT_INFO:
                g_message("PAM info: %s", msg[i]->msg);
                break;
            default:
                for (int j = 0; j < i; j++) free((*resp)[j].resp);
                free(*resp);
                *resp = NULL;
                return PAM_CONV_ERR;
        }
    }
    return PAM_SUCCESS;
}

/* Kill all processes owned by uid: SIGTERM, brief wait, then SIGKILL.
 * This purges orphaned X clients left after the session script exits,
 * restoring a clean display before the login window reappears. */
static void kill_user_processes(uid_t uid) {
    char uid_s[32];
    snprintf(uid_s, sizeof(uid_s), "%u", (unsigned)uid);
    pid_t p;

    p = fork();
    if (p == 0) { execlp("pkill", "pkill", "-TERM", "-u", uid_s, NULL); _exit(0); }
    if (p > 0)  waitpid(p, NULL, 0);

    usleep(500000); /* 0.5 s for processes to honour SIGTERM */

    p = fork();
    if (p == 0) { execlp("pkill", "pkill", "-KILL", "-u", uid_s, NULL); _exit(0); }
    if (p > 0)  waitpid(p, NULL, 0);
}

/* Clear the X root window to black.
 * Wallpaper tools (nitrogen, xfdesktop, pcmanfm, etc.) set a property on the
 * root window that persists after the process exits. Without this, the previous
 * user's wallpaper remains visible behind the login dialog. */
static void clear_root_window(void) {
    pid_t p = fork();
    if (p == 0) { execlp("xsetroot", "xsetroot", "-solid", "black", NULL); _exit(0); }
    if (p > 0)  waitpid(p, NULL, 0);
}

static void on_session_exit(GPid pid, gint status, gpointer data) {
    (void)status;
    g_spawn_close_pid(pid);

    const char *user = (const char *)data;

    if (pamh != NULL) {
        pam_close_session(pamh, 0);
        pam_end(pamh, PAM_SUCCESS);
        pamh = NULL;
    }

    /* Purge any X clients or other processes the session left behind */
    struct passwd *pw = getpwnam(user);
    if (pw && pw->pw_uid > 0)
        kill_user_processes(pw->pw_uid);

    clear_root_window();

    gtk_entry_set_text(GTK_ENTRY(password_entry), "");
    update_status("", FALSE);
    set_ui_sensitive(TRUE);
    gtk_widget_show_all(gtk_widget_get_toplevel(login_button));
    gtk_widget_grab_focus(username_entry);

    g_free(data);
}

static gboolean launch_session(gpointer user_data) {
    const char *user = (const char *)user_data;
    struct passwd *pw = getpwnam(user);

    if (!pw) {
        update_status("User not found", TRUE);
        set_ui_sensitive(TRUE);
        g_free(user_data);
        return G_SOURCE_REMOVE;
    }

    /* Ensure /run/user exists — not created by sysvinit on seatd-only systems */
    if (mkdir("/run/user", 0755) == -1 && errno != EEXIST) {
        update_status("Failed to create /run/user", TRUE);
        set_ui_sensitive(TRUE);
        g_free(user_data);
        return G_SOURCE_REMOVE;
    }

    /* Create /run/user/<uid> with correct ownership */
    char runtime_dir[64];
    snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%d", pw->pw_uid);
    if (mkdir(runtime_dir, 0700) == -1 && errno != EEXIST) {
        update_status("Failed to create runtime dir", TRUE);
        set_ui_sensitive(TRUE);
        g_free(user_data);
        return G_SOURCE_REMOVE;
    }
    if (chown(runtime_dir, pw->pw_uid, pw->pw_gid) != 0) { /* non-fatal */ }
    chmod(runtime_dir, 0700);

    pid_t pid = fork();
    if (pid < 0) {
        update_status("Fork failed", TRUE);
        set_ui_sensitive(TRUE);
        g_free(user_data);
        return G_SOURCE_REMOVE;
    }

    if (pid == 0) {
        /* ── child: become the user and exec their session ── */

        /* Reset signal handlers to defaults */
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        /* Close inherited file descriptors above stderr */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0) maxfd = 1024;
        for (long fd = 3; fd < maxfd; fd++) close((int)fd);

        /* Sanitise environment before dropping privileges */
        clearenv();

        /* Drop privileges — order matters: gid first, then uid */
        if (setgid(pw->pw_gid) != 0) _exit(1);
        if (initgroups(user, pw->pw_gid) != 0) _exit(1);
        if (setuid(pw->pw_uid) != 0) _exit(1);

        /* Build minimal, clean session environment */
        setenv("USER",            user,          1);
        setenv("LOGNAME",         user,          1);
        setenv("HOME",            pw->pw_dir,    1);
        setenv("SHELL",           pw->pw_shell,  1);
        setenv("PATH",            "/usr/local/bin:/usr/bin:/bin", 1);
        setenv("DISPLAY",         ":0",          1);
        setenv("XDG_RUNTIME_DIR", runtime_dir,   1);
        setenv("XDG_SEAT",        "seat0",       1);
        /* No XAUTHORITY — Xorg was started with -ac (no access control) */
        unsetenv("XAUTHORITY");

        if (chdir(pw->pw_dir) != 0) { if (chdir("/") != 0) { /* nowhere to go */ } }

        /* Launch session: ~/.xinitrc → system xinitrc → common WMs → xterm */
        char xinitrc[PATH_MAX];
        snprintf(xinitrc, sizeof(xinitrc), "%s/.xinitrc", pw->pw_dir);

        if (access(xinitrc, F_OK) == 0) {
            execl("/bin/sh", "sh", "--", xinitrc, NULL);
        } else if (access("/etc/X11/xinit/xinitrc", F_OK) == 0) {
            execl("/bin/sh", "sh", "--", "/etc/X11/xinit/xinitrc", NULL);
        } else {
            /* Last resort: try common WMs, then xterm */
            static const char *wms[] = {
                "jwm", "openbox-session", "startxfce4", "mate-session", "xterm", NULL
            };
            for (int i = 0; wms[i]; i++) {
                execlp(wms[i], wms[i], NULL);
                /* execlp returns only on failure — try the next one */
            }
        }
        _exit(127);
    }

    /* ── parent: hide UI and watch for child exit asynchronously ── */
    gtk_widget_hide(gtk_widget_get_toplevel(login_button));

    /* g_child_watch_add lets the GTK main loop keep running (processes
     * the hide event above) while waiting for the session to end. */
    g_child_watch_add(pid, on_session_exit, user_data);

    return G_SOURCE_REMOVE;
}

static void on_login_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const char *user = gtk_entry_get_text(GTK_ENTRY(username_entry));
    const char *pass = gtk_entry_get_text(GTK_ENTRY(password_entry));

    if (!*user || !*pass) {
        update_status("Enter username and password", TRUE);
        return;
    }

    if (strlen(user) > MAX_USERNAME) {
        update_status("Username too long", TRUE);
        return;
    }

    set_ui_sensitive(FALSE);
    update_status("Authenticating...", FALSE);

    struct pam_conv conv = { pam_conversation, NULL };
    int ret;

    ret = pam_start("xlogin", user, &conv, &pamh);
    if (ret != PAM_SUCCESS) {
        update_status(pam_strerror(NULL, ret), TRUE);
        set_ui_sensitive(TRUE);
        return;
    }

    const int flags = PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK;

    ret = pam_authenticate(pamh, flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    ret = pam_acct_mgmt(pamh, flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    ret = pam_setcred(pamh, PAM_ESTABLISH_CRED | flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    ret = pam_open_session(pamh, flags);
    if (ret != PAM_SUCCESS) {
        /* Credentials were established — must revoke them before ending PAM */
        pam_setcred(pamh, PAM_DELETE_CRED);
        goto auth_fail;
    }

    update_status("Starting session...", FALSE);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    launch_session,
                    g_strdup(user),
                    NULL);
    return;

auth_fail:
    update_status(pam_strerror(pamh, ret), TRUE);
    pam_end(pamh, ret);
    pamh = NULL;
    set_ui_sensitive(TRUE);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    if (entry == GTK_ENTRY(username_entry))
        gtk_widget_grab_focus(password_entry);
    else
        gtk_button_clicked(GTK_BUTTON(login_button));
}

static gboolean on_signal(gpointer data) {
    (void)data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    /* Use GLib's signal integration — async-signal-safe (self-pipe internally) */
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT,  on_signal, NULL);
    g_unix_signal_add(SIGHUP,  on_signal, NULL);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Login");
    gtk_window_set_default_size(GTK_WINDOW(window), 350, 220);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

#if GTK_MAJOR_VERSION >= 3
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
#else
    GtkWidget *box = gtk_vbox_new(FALSE, 12);
#endif
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);
    gtk_container_add(GTK_CONTAINER(window), box);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
                         "<span size=\"x-large\" weight=\"bold\">Login</span>");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    username_entry = gtk_entry_new();
#if GTK_MAJOR_VERSION >= 3
    gtk_entry_set_placeholder_text(GTK_ENTRY(username_entry), "Username");
#endif
    g_signal_connect(username_entry, "activate",
                     G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(box), username_entry, FALSE, FALSE, 0);

    password_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(password_entry), FALSE);
#if GTK_MAJOR_VERSION >= 3
    gtk_entry_set_placeholder_text(GTK_ENTRY(password_entry), "Password");
#endif
    g_signal_connect(password_entry, "activate",
                     G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(box), password_entry, FALSE, FALSE, 0);

    status_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

    login_button = gtk_button_new_with_label("Login");
    g_signal_connect(login_button, "clicked",
                     G_CALLBACK(on_login_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), login_button, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_widget_grab_focus(username_entry);

    gtk_main();
    return 0;
}
