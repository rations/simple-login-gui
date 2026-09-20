#!/bin/bash
# simple-login-gui installer.
#
# ONE FILE, TWO MODES, and it detects which it is in:
#
#   BINARY  -- a usr/ tree sits next to this script, because it was unpacked from a release
#              archive. Nothing is compiled; the tree is copied.
#   SOURCE  -- a Makefile sits next to it, because this is a clone or a source checkout.
#              Build dependencies are installed and it is built first.
#
# It is one file rather than two so that the installer shipped in the archive is the same
# installer that gets run during development. A separate "real" installer that only ever runs
# on someone else's machine is one nobody has tested.
#
# THE ORDER HERE IS A SAFETY PROPERTY, NOT A STYLE. /etc/inittab is edited LAST, and only after
# the binary has been proved to start on this machine. Getting that backwards means a machine
# whose tty1 getty has been replaced by a program that cannot run -- no login screen, no text
# console, nothing to read the error on. That is the one mistake this script must not make.

set -e

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: This script must be run as root" >&2
    exit 1
fi

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SELF_DIR"

PREFIX=/usr/local
BGDIR="$PREFIX/share/xlogin/backgrounds"

echo "=== simple-login-gui installer ==="
echo

# ── Which mode ────────────────────────────────────────────────────────────────
if [ -x "$SELF_DIR/usr/bin/xlogin" ] || [ -x "$SELF_DIR/usr/local/bin/xlogin" ]; then
    MODE=binary
    [ -x "$SELF_DIR/usr/local/bin/xlogin" ] && STAGED_BIN="$SELF_DIR/usr/local/bin/xlogin"
    [ -x "$SELF_DIR/usr/bin/xlogin" ]       && STAGED_BIN="$SELF_DIR/usr/bin/xlogin"
    echo "Installing from a prebuilt release archive."
elif [ -f "$SELF_DIR/Makefile" ] && [ -f "$SELF_DIR/src/main.cpp" ]; then
    MODE=source
    echo "Installing from source."
else
    echo "ERROR: this script is not next to a usr/ tree or a source tree." >&2
    echo "       Run it from inside the unpacked release archive, or from a checkout." >&2
    exit 1
fi
echo

# ── X server selection ───────────────────────────────────────────────────────
echo "Which X server are you using?"
echo "  1) XLibre (recommended for Devuan Excalibur)"
echo "  2) Xorg"
echo
while true; do
    read -p "Enter number [1-2]: " XSERVER_CHOICE
    case "$XSERVER_CHOICE" in
        1|2) break ;;
        *) echo "  Invalid choice. Enter 1 or 2." ;;
    esac
done

if [ "$XSERVER_CHOICE" = "1" ]; then
    if command -v Xlibre > /dev/null 2>&1; then
        echo "  Found: Xlibre at $(command -v Xlibre)"
    elif command -v Xorg > /dev/null 2>&1; then
        echo "  Note: Xlibre binary not found in PATH."
        echo "  XLibre may be installed as 'Xorg' on some systems (it replaces xorg-server)."
        echo "  Found: Xorg at $(command -v Xorg) — this will be used as the X server."
    else
        echo
        echo "  WARNING: No X server binary found."
        echo "  Install XLibre from the Devuan XLibre repository before rebooting."
        echo "  Installation will continue but the login screen will not work"
        echo "  until an X server is installed."
    fi
else
    if command -v Xorg > /dev/null 2>&1; then
        echo "  Found: Xorg at $(command -v Xorg)"
    else
        echo
        echo "  WARNING: Xorg not found. Install it before rebooting."
    fi
fi
echo

# ── GPU detection: configure seatd integration ────────────────────────────────
echo "Detecting GPU driver..."
XSERVER_FLAGS="-seat seat0 -keeptty -nolisten tcp -ac"

if lsmod 2>/dev/null | grep -q '^nvidia '; then
    echo "  NVIDIA proprietary driver detected."
    echo "  The nvidia DDX driver does not support libseat device management."
    echo "  seatd integration will be disabled for the X server."
    echo "  seatd will still run and is available for Wayland compositors."
    XSERVER_FLAGS="-nolisten tcp -ac"
else
    echo "  Open-source GPU driver detected. Full seatd integration enabled."
fi
echo

# ── Console VT ────────────────────────────────────────────────────────────────
# The Options menu has a Console entry that switches to a text VT. Switching to a VT
# with nothing running on it gives a black screen with a cursor, which looks exactly
# like a crash -- so the default is checked against what inittab actually respawns.
CONSOLE_VT=2
if ! grep -qE "^[^#].*getty.*\btty${CONSOLE_VT}\b" /etc/inittab 2>/dev/null; then
    FOUND_VT=""
    for V in 2 3 4 5 6; do
        if grep -qE "^[^#].*getty.*\btty${V}\b" /etc/inittab 2>/dev/null; then
            FOUND_VT="$V"
            break
        fi
    done
    if [ -n "$FOUND_VT" ]; then
        CONSOLE_VT="$FOUND_VT"
        echo "No getty on tty2; the Console entry will use tty${CONSOLE_VT} instead."
    else
        echo "WARNING: no getty appears to respawn on tty2-tty6 in /etc/inittab."
        echo "  The Console entry in the Options menu will switch to tty2 and show"
        echo "  a blank screen. Add a getty line to /etc/inittab, or change"
        echo "  XLOGIN_CONSOLE_VT in /etc/xlogin.conf afterwards."
    fi
    echo
fi

# ── Write /etc/xlogin.conf, PRESERVING anything already set ───────────────────
# The login screen writes XLOGIN_BACKGROUND here itself when somebody picks a
# background from the Options menu. Re-running the installer must not throw that
# away, so existing xlogin settings are read back before the file is rewritten.
OLD_BACKGROUND=""
OLD_BG_MODE=""
OLD_CONSOLE_VT=""
if [ -f /etc/xlogin.conf ]; then
    # shellcheck disable=SC1091
    OLD_BACKGROUND=$(sh -c '. /etc/xlogin.conf 2>/dev/null; printf "%s" "${XLOGIN_BACKGROUND:-}"')
    OLD_BG_MODE=$(sh -c '. /etc/xlogin.conf 2>/dev/null; printf "%s" "${XLOGIN_BG_MODE:-}"')
    OLD_CONSOLE_VT=$(sh -c '. /etc/xlogin.conf 2>/dev/null; printf "%s" "${XLOGIN_CONSOLE_VT:-}"')
    cp /etc/xlogin.conf "/etc/xlogin.conf.backup.$(date +%Y%m%d_%H%M%S)"
fi
[ -n "$OLD_CONSOLE_VT" ] && CONSOLE_VT="$OLD_CONSOLE_VT"
[ -z "$OLD_BG_MODE" ] && OLD_BG_MODE="fill"

cat > /etc/xlogin.conf <<EOF
# xlogin configuration.
#
# This file is sourced by /bin/sh (by xlogin-launcher, as root, at boot), so every
# line must be a plain KEY='value'. xlogin rewrites single keys here in place and
# leaves everything else alone, so anything you add by hand survives.

# Passed to the X server by xlogin-launcher at startup. seatd integration
# (-seat seat0 -keeptty) requires an open-source GPU driver; the nvidia
# proprietary DDX driver does not support libseat device management.
XSERVER_FLAGS="$XSERVER_FLAGS"

# The VT the Options > Console entry switches to. There must be a getty on it.
XLOGIN_CONSOLE_VT='$CONSOLE_VT'

# Background image: a filename inside $BGDIR, or
# empty for none. Set from the Options menu, or here.
XLOGIN_BACKGROUND='$OLD_BACKGROUND'

# fill | fit | center | stretch | tile
XLOGIN_BG_MODE='$OLD_BG_MODE'
EOF
echo "  Wrote /etc/xlogin.conf (Console will use tty${CONSOLE_VT})"
echo

# ── Runtime dependencies ──────────────────────────────────────────────────────
echo "Installing runtime dependencies..."
apt-get update -q
apt-get install -y libcairo2 libfreetype6 libjpeg62-turbo libx11-6 libxrandr2 \
                   libpam0g seatd libseat1 x11-xserver-utils xinit xterm
echo "  done."
echo

# ── Build (source mode only) ─────────────────────────────────────────────────
if [ "$MODE" = source ]; then
    echo "Installing build dependencies..."
    apt-get install -y libcairo2-dev libfreetype-dev libjpeg-dev libx11-dev \
                       libxrandr-dev libpam0g-dev build-essential g++ make
    echo "Building..."
    make
    echo "  build complete."
    echo
    STAGED_BIN="$SELF_DIR/xlogin"
fi

# ── PROVE THE BINARY RUNS, BEFORE ANYTHING IRREVERSIBLE ──────────────────────
# This is the check only the recipient's machine can make, and it is why the runtime
# dependencies went in above this line and /etc/inittab is edited below it.
#
# A release binary carries the glibc symbol versions of the machine that built it. On an
# older distribution the loader fails with a message about a symbol version, and if that
# happens AFTER inittab has been changed there is no login screen and no getty on tty1 to
# read the message on. So: run it here, and refuse to go on if it will not start.
echo "Checking the binary runs on this machine..."
if ! VERSION_OUT=$("$STAGED_BIN" --version 2>&1); then
    echo >&2
    echo "ERROR: $STAGED_BIN will not start on this machine:" >&2
    echo "  $VERSION_OUT" >&2
    echo >&2
    if printf '%s' "$VERSION_OUT" | grep -q 'GLIBC'; then
        echo "That is a glibc version error: this archive was built on a newer" >&2
        echo "distribution than this one. Build from source instead --" >&2
        echo "the source release installs the same way." >&2
    fi
    echo >&2
    echo "NOTHING HAS BEEN CHANGED that stops this machine booting: /etc/inittab" >&2
    echo "has not been touched and the tty1 getty is still in place." >&2
    exit 1
fi
echo "  $VERSION_OUT"
echo

# ── Install the files ─────────────────────────────────────────────────────────
echo "Installing binaries, fonts and config..."
if [ "$MODE" = binary ]; then
    # The archive is an absolute tree. Copy it as-is: the paths inside the binary were
    # compiled for exactly these locations, which is why the archive is not relocatable.
    for TREE in usr etc; do
        [ -d "$SELF_DIR/$TREE" ] || continue
        cp -a --no-preserve=ownership "$SELF_DIR/$TREE/." "/$TREE/"
    done
    chown -R root:root "$PREFIX/share/xlogin" "$PREFIX/bin/xlogin" "$PREFIX/bin/xlogin-launcher"
    chmod 755 "$PREFIX/bin/xlogin" "$PREFIX/bin/xlogin-launcher" /etc/init.d/xlogin-launcher
else
    # make install puts every file where it goes and deliberately touches nothing else.
    make install
fi
install -d -m 755 "$BGDIR"
echo "  done."
echo

# ── Enable seatd at boot ──────────────────────────────────────────────────────
echo "Enabling seatd service..."
LC_ALL=C update-rc.d seatd defaults
echo "  done."
echo

# ── Target user ───────────────────────────────────────────────────────────────
read -p "Username to configure for graphical login: " TARGET_USER
if ! id "$TARGET_USER" > /dev/null 2>&1; then
    echo "ERROR: user '$TARGET_USER' does not exist." >&2
    exit 1
fi
USER_HOME=$(getent passwd "$TARGET_USER" | cut -d: -f6)

echo "Adding $TARGET_USER to input, video, and plugdev groups..."
usermod -aG input,video,plugdev "$TARGET_USER"
echo "  done."
echo

# ── Detect installed sessions ─────────────────────────────────────────────────
echo "Detecting installed sessions..."

SESSION_NAMES=()
SESSION_EXECS=()

if [ -d /usr/share/xsessions ]; then
    for desktop in /usr/share/xsessions/*.desktop; do
        [ -f "$desktop" ] || continue
        NAME=$(grep '^Name=' "$desktop" | head -1 | cut -d= -f2-)
        EXEC=$(grep '^Exec=' "$desktop" | head -1 | cut -d= -f2-)
        [ -z "$NAME" ] && continue
        [ -z "$EXEC" ] && continue
        SESSION_NAMES+=("$NAME")
        SESSION_EXECS+=("$EXEC")
    done
fi

CHOSEN_CMD=""

if [ ${#SESSION_NAMES[@]} -gt 0 ]; then
    echo "  Found:"
    for i in "${!SESSION_NAMES[@]}"; do
        echo "  $((i+1))) ${SESSION_NAMES[$i]}"
    done
    echo
    while true; do
        read -p "Enter number [1-${#SESSION_NAMES[@]}]: " CHOICE
        if [[ "$CHOICE" =~ ^[0-9]+$ ]] && \
           [ "$CHOICE" -ge 1 ] && [ "$CHOICE" -le "${#SESSION_NAMES[@]}" ]; then
            CHOSEN_CMD="${SESSION_EXECS[$((CHOICE-1))]}"
            break
        fi
        echo "  Invalid choice."
    done
else
    echo "  No sessions found in /usr/share/xsessions/."
    echo "  Install a window manager or desktop environment first, then re-run,"
    echo "  or enter a session command manually (leave blank to abort):"
    read -p "  Session command: " CHOSEN_CMD
    if [ -z "$CHOSEN_CMD" ]; then
        echo "ERROR: no session command provided." >&2
        exit 1
    fi
fi

echo "Using session command: $CHOSEN_CMD"
echo

# ── D-Bus session ─────────────────────────────────────────────────────────────
echo "Start the session inside a D-Bus session bus?"
echo "  Enables trash, removable media, and other GVfs-backed features"
echo "  in file managers such as pcmanfm. Uses dbus-run-session."
echo
read -p "Add dbus-run-session to ~/.xinitrc? [Y/n]: " DBUS_CHOICE
case "$DBUS_CHOICE" in
    [nN]*) USE_DBUS=no ;;
    *)     USE_DBUS=yes ;;
esac

if [ "$USE_DBUS" = "yes" ]; then
    if ! command -v dbus-run-session > /dev/null 2>&1; then
        echo "  Installing dbus..."
        apt-get install -y dbus
    fi
    SESSION_EXEC="dbus-run-session $CHOSEN_CMD"
else
    SESSION_EXEC="$CHOSEN_CMD"
fi
echo "  Session command: $SESSION_EXEC"
echo

# ── Write ~/.xinitrc ──────────────────────────────────────────────────────────
XINITRC="$USER_HOME/.xinitrc"
WRITE_XINITRC=yes

if [ -f "$XINITRC" ]; then
    echo "  $XINITRC already exists."
    read -p "  Overwrite it? [y/N]: " OW
    case "$OW" in
        [yY]*) WRITE_XINITRC=yes ;;
        *)     WRITE_XINITRC=no ;;
    esac
fi

if [ "$WRITE_XINITRC" = "yes" ]; then
    cat > "$XINITRC" <<EOF
#!/bin/sh
exec $SESSION_EXEC
EOF
    chown "$TARGET_USER:$TARGET_USER" "$XINITRC"
    chmod 755 "$XINITRC"
    echo "  Wrote $XINITRC"
fi

# Also install to /etc/skel so new users get a working .xinitrc
cat > /etc/skel/.xinitrc <<EOF
#!/bin/sh
exec $SESSION_EXEC
EOF
chmod 755 /etc/skel/.xinitrc
echo "  Wrote /etc/skel/.xinitrc"
echo

# ── Background image ──────────────────────────────────────────────────────────
# Images live in ONE root-owned directory and nowhere else. xlogin refuses to read
# anything from it that is not a regular file owned by uid 0 and not group- or
# world-writable, and it decides the format by the magic bytes, not the extension.
# That is not paperwork: the decoder runs as root before anybody has authenticated,
# so the only people who can hand it a file must already be root.
echo "Background image for the login screen (optional)."
echo "  PNG or JPEG. It is copied into $BGDIR"
echo "  and owned by root -- you can add or change it later from the Options menu"
echo "  on the login screen, or by copying files there by hand."
echo
read -p "Path to an image, or blank for a plain background: " BG_SRC

if [ -n "$BG_SRC" ]; then
    if [ ! -f "$BG_SRC" ]; then
        echo "  WARNING: $BG_SRC is not a file. Skipping; the login screen will use a"
        echo "           plain background. You can add one later from the Options menu."
    else
        BG_NAME=$(basename "$BG_SRC")
        case "$BG_NAME" in
            *.png|*.PNG|*.jpg|*.JPG|*.jpeg|*.JPEG) ;;
            *)
                echo "  Note: '$BG_NAME' does not end in .png or .jpg. That is fine --"
                echo "  xlogin decides the format from the file's contents, not its name."
                ;;
        esac
        install -d -m 755 "$BGDIR"
        install -m 644 -o root -g root "$BG_SRC" "$BGDIR/$BG_NAME"
        # Written through a temp file and a rename, the same way xlogin writes it.
        TMPCONF=$(mktemp /etc/xlogin.conf.tmpXXXXXX)
        sed "s|^XLOGIN_BACKGROUND=.*|XLOGIN_BACKGROUND='$BG_NAME'|" /etc/xlogin.conf > "$TMPCONF"
        chmod 644 "$TMPCONF"
        mv "$TMPCONF" /etc/xlogin.conf
        echo "  Installed $BGDIR/$BG_NAME and set it as the background."
    fi
    echo
fi

# ── Configure inittab ─────────────────────────────────────────────────────────
# LAST, and deliberately so: everything above can be undone by re-running this script or
# by deleting a file. This is the step that decides what happens at boot, and it is only
# reached because the binary was proved to start further up.
if ! grep -q xlogin-launcher /etc/inittab 2>/dev/null; then
    echo "Updating /etc/inittab..."
    cp /etc/inittab "/etc/inittab.backup.$(date +%Y%m%d_%H%M%S)"
    sed -i '/^1:[0-9]*:respawn:.*[ag]etty/s/^/#/' /etc/inittab
    echo "1:2345:respawn:$PREFIX/bin/xlogin-launcher" >> /etc/inittab
    telinit q
    echo "  done."
else
    echo "inittab already configured."
fi
echo

echo "=== Installation complete ==="
echo
echo "Notes:"
echo "  - Reboot to activate the graphical login screen."
echo "    Logging out of a graphical session returns you to the current display"
echo "    manager, not xlogin. A full reboot is required."
echo "  - Users can customise their session by editing ~/.xinitrc"
echo "  - To add another user, re-run this script or manually:"
echo "      usermod -aG input,video <username>"
echo "      cp /etc/skel/.xinitrc /home/<username>/.xinitrc"
echo "      chown <username>:<username> /home/<username>/.xinitrc"
echo
echo "  - The login screen has an Options button: Console, Background, Restart"
echo "    and Shut down. Restart and Shut down ask for a second click. Console"
echo "    switches to tty${CONSOLE_VT} and leaves X running, so Alt+F1 comes back."
echo "  - Anyone at the keyboard can use those entries WITHOUT logging in. That is"
echo "    deliberate -- they can already hold the power button in -- but if this"
echo "    machine is somewhere the difference matters, see the Threat model"
echo "    section of README.md before you rely on it."
echo "  - To add more background images later:"
echo "      sudo install -m 644 -o root -g root <image> $BGDIR/"
