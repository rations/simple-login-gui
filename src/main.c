#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <security/pam_appl.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

extern char **environ;

static GtkWidget *username_entry;
static GtkWidget *password_entry;
static GtkWidget *status_label;
static GtkWidget *login_button;
static pam_handle_t *pamh;

static void update_status(const char *text, gboolean error) {
    gtk_label_set_text(GTK_LABEL(status_label), text);
    if (error) {
        gtk_widget_set_name(status_label, "error-label");
    } else {
        gtk_widget_set_name(status_label, "status-label");
    }
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
                g_warning("PAM Error: %s", msg[i]->msg);
                break;
            case PAM_TEXT_INFO:
                g_message("PAM Info: %s", msg[i]->msg);
                break;
            default:
                for (int j = 0; j < i; j++) free((*resp)[j].resp);
                free(*resp);
                return PAM_CONV_ERR;
        }
    }
    return PAM_SUCCESS;
}

static gboolean launch_session(gpointer user_data) {
    const char *user = (const char *)user_data;
    struct passwd *pw = getpwnam(user);
    
    if (!pw) {
        update_status("User not found", TRUE);
        set_ui_sensitive(TRUE);
        return G_SOURCE_REMOVE;
    }

    // Create XDG_RUNTIME_DIR
    char runtime_dir[64];
    snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%d", pw->pw_uid);
    
    if (mkdir(runtime_dir, 0700) == -1 && errno != EEXIST) {
        _exit(1);
    }
    if (chown(runtime_dir, pw->pw_uid, pw->pw_gid) == -1) {
        _exit(1);
    }
    if (chmod(runtime_dir, 0700) == -1) {
        _exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        // Reset all signal handlers
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        // Close all inherited file descriptors
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd == -1) maxfd = 1024;
        for (long fd = 3; fd < maxfd; fd++) {
            int r;
            do {
                r = close(fd);
            } while (r == -1 && errno == EINTR);
        }

        // Clean environment BEFORE privilege drop
        clearenv();
        
        // CORRECT ORDER: setgid FIRST, then initgroups, then setuid
        if (setgid(pw->pw_gid) != 0) _exit(1);
        if (initgroups(user, pw->pw_gid) != 0) _exit(1);
        if (setuid(pw->pw_uid) != 0) _exit(1);

        setenv("USER", user, 1);
        setenv("LOGNAME", user, 1);
        setenv("HOME", pw->pw_dir, 1);
        setenv("SHELL", pw->pw_shell, 1);
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
        setenv("DISPLAY", ":0", 1);
        setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
        setenv("LIBSEAT_BACKEND", "seatd", 1);

        chdir(pw->pw_dir);

        execve("/usr/bin/startx", (char *const[]) { "startx", NULL }, environ);
        _exit(127);
    } else if (pid > 0) {
        // Parent process
        update_status("Starting session...", FALSE);
        gtk_widget_hide(gtk_widget_get_toplevel(login_button));

        int status;
        waitpid(pid, &status, 0);

        // Session ended, redisplay login
        update_status("", FALSE);
        // Cleanup PAM session after user logged out
        pam_end(pamh, PAM_SUCCESS);
        
        // Securely wipe password from memory
        gtk_entry_set_text(GTK_ENTRY(password_entry), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
        gtk_entry_set_text(GTK_ENTRY(password_entry), "");
        gtk_widget_show(gtk_widget_get_toplevel(login_button));
        set_ui_sensitive(TRUE);
        gtk_widget_grab_focus(username_entry);
    }

    return G_SOURCE_REMOVE;
}

static void on_login_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    const char *user = gtk_entry_get_text(GTK_ENTRY(username_entry));
    const char *pass = gtk_entry_get_text(GTK_ENTRY(password_entry));

    if (!*user || !*pass) {
        update_status("Please enter username and password", TRUE);
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

    const int pam_flags = PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK;
    
    ret = pam_authenticate(pamh, pam_flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    ret = pam_acct_mgmt(pamh, pam_flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    ret = pam_setcred(pamh, PAM_ESTABLISH_CRED | pam_flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    ret = pam_open_session(pamh, pam_flags);
    if (ret != PAM_SUCCESS) goto auth_fail;

    update_status("Authentication successful", FALSE);

    // Keep PAM session will be closed after session exits in waitpid
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, (GSourceFunc)launch_session, g_strdup(user), g_free);
    return;

auth_fail:
    update_status(pam_strerror(pamh, ret), TRUE);
    pam_end(pamh, ret);
    set_ui_sensitive(TRUE);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    
    if (entry == GTK_ENTRY(username_entry)) {
        gtk_widget_grab_focus(password_entry);
    } else {
        gtk_button_clicked(GTK_BUTTON(login_button));
    }
}

static void sig_handler(int sig) {
    (void)sig;
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Login");
    gtk_window_set_default_size(GTK_WINDOW(window), 350, 200);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);
    gtk_container_add(GTK_CONTAINER(window), box);

    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label), "<span size=\"x-large\" weight=\"bold\">Login</span>");
    gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);

    username_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(username_entry), "Username");
    g_signal_connect(username_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(box), username_entry, FALSE, FALSE, 0);

    password_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(password_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(password_entry), "Password");
    g_signal_connect(password_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(box), password_entry, FALSE, FALSE, 0);

    status_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

    login_button = gtk_button_new_with_label("Login");
    g_signal_connect(login_button, "clicked", G_CALLBACK(on_login_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), login_button, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_widget_grab_focus(username_entry);

    gtk_main();

    return 0;
}
