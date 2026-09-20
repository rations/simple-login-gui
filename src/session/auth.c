/* See auth.h. */

#define _GNU_SOURCE
#include "auth.h"

#include <security/pam_appl.h>

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
 * field's fixed buffer, which the caller erases. */
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
                out[i].resp = strdup(data->password ? data->password : "");
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

auth_result auth_login(const char *user, const char *password)
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

    if (!user || !*user || !password) {
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

    ret = pam_start("xlogin", user, &conv, &g_pamh);
    if (ret != PAM_SUCCESS) {
        /* pam_start failing means there is no handle, so pam_strerror gets NULL -- which it
         * accepts, falling back to the generic text for the code. */
        set_message(NULL, ret);
        g_pamh = NULL;
        r.code = ret;
        return r;
    }

    ret = pam_authenticate(g_pamh, flags);
    if (ret != PAM_SUCCESS)
        goto fail;

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
