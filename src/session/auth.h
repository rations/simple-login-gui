/* PAM authentication.
 *
 * Plain C, behind extern "C". Everything in this directory runs as root, and launch.c runs
 * across a fork, so the whole session half stays in the language with no destructors, no
 * exceptions and no allocation the code did not ask for.
 *
 * Lifted from the GTK implementation's on_login_clicked() with the flag combinations and the
 * ordering intact. What changed is only where the password comes from -- a fixed buffer owned
 * by the password field instead of a GtkEntry -- and that the errors are returned rather than
 * written into a widget.
 *
 * THE RETURN-VALUE CONTRACT THAT IS EASY TO DROP: once pam_setcred(PAM_ESTABLISH_CRED) has
 * succeeded, there is an obligation to pam_setcred(PAM_DELETE_CRED) on any later failure and
 * at the end of the session, before pam_end. Skipping it leaves credentials established for a
 * session that never started. It is invisible until an account expires mid-login, which is
 * exactly the kind of thing that survives a refactor by not being noticed.
 */

#ifndef XLOGIN_SESSION_AUTH_H
#define XLOGIN_SESSION_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ok;   /* non-zero when the user is authenticated and a PAM session is open */
    int code; /* the PAM return code, PAM_SUCCESS on success */
    /* Human-readable, already run through pam_strerror, and safe to display. Points at storage
     * owned by this module that lives until the next auth_login(), so the caller may show it
     * but must not keep it. Never contains anything the user typed. */
    const char *message;
} auth_result;

/* Authenticate and open a session. `password` is the plaintext, which is handed to PAM and to
 * nothing else -- it is not copied, logged or retained here beyond the single strdup into the
 * pam_response that PAM itself frees (cites: _pam_types.h:274 -- the response structure is
 * "allocated by the application program, and free()'d by the Linux-PAM library").
 *
 * On success the PAM handle stays open and auth_close_session() must eventually be called. On
 * failure everything is already torn down and there is nothing to close. */
auth_result auth_login(const char *user, const char *password);

/* Open a session for `user` WITHOUT a password, through the xlogin-autologin PAM service.
 *
 * For the automatic login only. The caller is trusted to have checked that `user` came from
 * XLOGIN_AUTOLOGIN in a root-owned, root-only-writable /etc/xlogin.conf, which is only ever
 * written with a name after that user has authenticated through auth_login(). It still refuses
 * an unknown user and uid 0 itself, before pam_start, because this is the function that skips
 * the password and it should not depend on its caller for that. pam_acct_mgmt still runs, so
 * an expired or locked account is refused here exactly as it is at the password prompt.
 *
 * Success and failure leave the same state as auth_login(): on success, auth_close_session()
 * must eventually be called. */
auth_result auth_autologin(const char *user);

/* Close the session opened by a successful auth_login() or auth_autologin(): pam_close_session,
 * then pam_setcred(PAM_DELETE_CRED), then pam_end, in that order. A no-op when nothing is open, so
 * it is safe on every exit path. */
void auth_close_session(void);

/* Non-zero while a PAM session is open. */
int auth_session_open(void);

#ifdef __cplusplus
}
#endif

#endif /* XLOGIN_SESSION_AUTH_H */
