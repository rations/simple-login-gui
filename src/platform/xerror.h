// Non-fatal X error handling.
//
// Xlib's default error handler calls exit(). This process is the login screen on tty1: there is
// no desktop behind it and no session manager to restart it, so one BadWindow -- a stale
// resource id, a race with a client that is going away -- taking the process down means the
// machine shows a black screen and the person in front of it has no way in. Init will respawn
// the launcher eventually, but the X server has to come back up first and the user has no idea
// why any of it happened. Swallowing the error and logging it is strictly better.
//
// Ported from the CPU-Power window, which took it from the rations-amp plug-in's
// x11plugview.cpp, where it exists because a plug-in that calls exit() takes the HOST down with
// it. The mechanism is the same; only the stakes differ.
//
// Errors on a display that is not ours are forwarded to whatever handler was installed before,
// so this never swallows another library's diagnostics.

#pragma once

#include <X11/Xlib.h>

namespace xlogin
{

// Install the process-wide handler (once) and start treating errors on `display` as ours.
void registerDisplay(::Display *display);
void unregisterDisplay(::Display *display);

// X requests are asynchronous, so a rejected XCreateWindow does not fail in place: the only way
// to find out is to sample this, round-trip with XSync, and sample it again. That is what makes
// asynchronous window-creation failure detectable at all.
unsigned long errorCount();

} // namespace xlogin
