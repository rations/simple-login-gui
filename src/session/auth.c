/* See auth.h. */

#define _GNU_SOURCE
#include "auth.h"

#include <security/pam_appl.h>

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* The handle for the session currently open, or NULL. One at a time: this program runs one
 * login screen and starts one session. */
static pam_handle_t *g_pamh = NULL;

/* Set once pam_setcred(PAM_ESTABLISH_CRED) has succeeded, cleared once the credentials have
 * been deleted. This is the flag the obligation described in auth.h hangs on. */
static int g_cred_established = 0;

/* pam_strerror's text, copied. The pointer PAM returns is documented as valid, but copying it
 * removes the question of what it points at after pam_end and bounds it at a length the status
 * line can show. Never holds anything the user typed. */
static char g_message[256];

/* What the conversation function answers with. Both are borrowed for the duration of one
 * auth_login() call and are not owned here -- in particular `password` points into the text
 * field's fixed buffer, which the caller erases. `password` is NULL for an automatic login, and
 * a module that asks for one then is answered with a conversation error, not an empty string:
 * the xlogin-autologin stack is not supposed to ask, and if somebody has edited it so that it
 * does, failing is the answer that does not let an empty password through. */
struct conv_data {
    const char *user;
    const char *password;
};

static void set_message(pam_handle_t *pamh, int code)
{
    const char *s = pam_strerror(pamh, code);
    if (!s)
        s = "Authentication failed";
    snprintf(g_message, sizeof(g_message), "%s", s);
}

/* The conversation.
 *
 * PAM_PROMPT_ECHO_OFF is the password and PAM_PROMPT_ECHO_ON the username. The prompts
 * themselves are ignored: this is a graphical login with two fields, and there is nothing to
 * do with a module that asks a third question except fail, which is what returning
 * PAM_CONV_ERR below does.
 *
 * PAM_ERROR_MSG and PAM_TEXT_INFO go to syslog and NOT to the screen. A module's text can name
 * the account, say whether it exists, or say how many attempts are left, and none of that is
 * something to tell somebody who has not yet proved who they are.
 */
static int conversation(int num_msg, const struct pam_message **msg, struct pam_response **resp,
                        void *appdata_ptr)
{
    const struct conv_data *data = (const struct conv_data *)appdata_ptr;
    struct pam_response *out;
    int i;

    if (!data || num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
        return PAM_CONV_ERR;

    /* calloc, not malloc: a response the loop below does not fill must be a NULL pointer, not
     * an uninitialised one that PAM will try to free. */
    out = calloc((size_t)num_msg, sizeof(struct pam_response));
    if (!out)
        return PAM_BUF_ERR;

    for (i = 0; i < num_msg; i++) {
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
                if (!data->password)
                    goto conv_error;
                out[i].resp = strdup(data->password);
                break;
            case PAM_PROMPT_ECHO_ON:
                out[i].resp = strdup(data->user ? data->user : "");
                break;
            case PAM_ERROR_MSG:
                syslog(LOG_AUTHPRIV | LOG_WARNING, "pam: %s", msg[i]->msg ? msg[i]->msg : "");
                break;
            case PAM_TEXT_INFO:
                syslog(LOG_AUTHPRIV | LOG_INFO, "pam: %s", msg[i]->msg ? msg[i]->msg : "");
                break;
            default:
                goto conv_error;
        }

        /* strdup failing is out of memory, and an out-of-memory login must fail rather than
         * hand PAM a NULL response it will dereference. */
        if ((msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) &&
            !out[i].resp)
            goto conv_error;
    }

    *resp = out;
    return PAM_SUCCESS;

conv_error:
    /* Erase before freeing: one of these may be the password, and free() does not zero. */
    for (i = 0; i < num_msg; i++) {
        if (out[i].resp) {
            explicit_bzero(out[i].resp, strlen(out[i].resp));
            free(out[i].resp);
        }
    }
    free(out);
    *resp = NULL;
    return PAM_CONV_ERR;
}

int auth_session_open(void)
{
    return g_pamh != NULL;
}

/* The one path into a PAM session. auth_login() and auth_autologin() differ only in which
 * service they name and whether pam_authenticate runs, so everything else -- the account
 * check, the credentials and the obligation that comes with them, the seat environment, the
 * failure path -- is this code, once, rather than two copies that can drift apart. */
static auth_result auth_begin(const char *service, const char *user, const char *password,
                              int do_auth)
{
    auth_result r;
    struct conv_data data;
    struct pam_conv conv;
    const int flags = PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK;
    const char *vtnr;
    int ret;

    r.ok = 0;
    r.code = PAM_SYSTEM_ERR;
    r.message = g_message;
    g_message[0] = '\0';

    if (!user || !*user || (do_auth && !password)) {
        snprintf(g_message, sizeof(g_message), "Enter a username and password");
        return r;
    }

    /* One at a time. A second login while a session is open is a bug in the caller, not
     * something to paper over by leaking the first handle. */
    if (g_pamh) {
        snprintf(g_message, sizeof(g_message), "A session is already open");
        return r;
    }

    data.user = user;
    data.password = password;
    conv.conv = conversation;
    conv.appdata_ptr = &data;

    ret = pam_start(service, user, &conv, &g_pamh);
    if (ret != PAM_SUCCESS) {
        /* pam_start failing means there is no handle, so pam_strerror gets NULL -- which it
         * accepts, falling back to the generic text for the code. */
        set_message(NULL, ret);
        g_pamh = NULL;
        r.code = ret;
        return r;
    }

    /* Skipped ONLY for an automatic login, whose stack is xlogin-autologin and whose user is
     * one who authenticated through the xlogin stack when they turned it on. The account
     * check below is NOT skipped: an expired or locked account is refused here too. */
    if (do_auth) {
        ret = pam_authenticate(g_pamh, flags);
        if (ret != PAM_SUCCESS)
            goto fail;
    }

    ret = pam_acct_mgmt(g_pamh, flags);
    if (ret != PAM_SUCCESS)
        goto fail;

    ret = pam_setcred(g_pamh, PAM_ESTABLISH_CRED | flags);
    if (ret != PAM_SUCCESS)
        goto fail;
    g_cred_established = 1;

    /* Give pam_elogind the seat and VT context it needs to register the session as ACTIVE at
     * the seat. Without XDG_VTNR the session registers but is not active, and polkit then
     * demands root for mounting a disk or shutting down from inside the session. Carried over
     * from the GTK implementation, where this was found the hard way.
     *
     * XDG_VTNR comes from the launcher's environment, which is this process's own -- not from
     * anything the user typed. */
    (void)pam_putenv(g_pamh, "XDG_SEAT=seat0");
    (void)pam_putenv(g_pamh, "XDG_SESSION_TYPE=x11");
    (void)pam_putenv(g_pamh, "XDG_SESSION_CLASS=user");
    vtnr = getenv("XDG_VTNR");
    if (vtnr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "XDG_VTNR=%s", vtnr);
        (void)pam_putenv(g_pamh, buf);
    }

    ret = pam_open_session(g_pamh, flags);
    if (ret != PAM_SUCCESS)
        goto fail;

    r.ok = 1;
    r.code = PAM_SUCCESS;
    snprintf(g_message, sizeof(g_message), "Starting session...");
    return r;

fail:
    set_message(g_pamh, ret);
    r.code = ret;
    /* The obligation from auth.h: credentials that were established are deleted before the
     * handle goes away, whatever it was that failed afterwards. */
    if (g_cred_established) {
        (void)pam_setcred(g_pamh, PAM_DELETE_CRED);
        g_cred_established = 0;
    }
    pam_end(g_pamh, ret);
    g_pamh = NULL;
    return r;
}

auth_result auth_login(const char *user, const char *password)
{
    return auth_begin("xlogin", user, password, 1);
}

auth_result auth_autologin(const char *user)
{
    auth_result r;
    const struct passwd *pw;

    r.ok = 0;
    r.code = PAM_USER_UNKNOWN; /* cites: security/_pam_types.h:43 */
    r.message = g_message;

    /* Refused before PAM is even started. The caller has already checked both, but this is the
     * function that skips the password, so it is the one that must not trust that it was. */
    pw = user && *user ? getpwnam(user) : NULL;
    if (!pw) {
        snprintf(g_message, sizeof(g_message), "Automatic login: user not found");
        return r;
    }
    if (pw->pw_uid == 0) {
        snprintf(g_message, sizeof(g_message), "Automatic login is not allowed for root");
        r.code = PAM_PERM_DENIED; /* cites: security/_pam_types.h:36 */
        return r;
    }

    return auth_begin("xlogin-autologin", user, NULL, 0);
}

void auth_close_session(void)
{
    if (!g_pamh)
        return;

    (void)pam_close_session(g_pamh, 0);
    if (g_cred_established) {
        (void)pam_setcred(g_pamh, PAM_DELETE_CRED);
        g_cred_established = 0;
    }
    pam_end(g_pamh, PAM_SUCCESS);
    g_pamh = NULL;
}
