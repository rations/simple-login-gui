#!/bin/bash
set -e

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: This script must be run as root" >&2
    exit 1
fi

echo "=== simple-login-gui installer ==="
echo

# ── Runtime dependencies ──────────────────────────────────────────────────────
echo "Installing runtime dependencies..."
apt-get update -q
apt-get install -y libgtk-3-0 libpam0g seatd libseat1 x11-xserver-utils xinit xterm
echo "  done."
echo

# ── Check for X server ───────────────────────────────────────────────────────
echo "Checking for X server..."
if command -v Xlibre > /dev/null 2>&1; then
    echo "  Found: Xlibre"
elif command -v Xorg > /dev/null 2>&1; then
    echo "  Found: Xorg"
else
    echo
    echo "  WARNING: No X server found (Xlibre or Xorg)."
    echo "  xlogin-launcher requires XLibre (recommended for Devuan Excalibur)."
    echo "  Install it from the Devuan XLibre repository before rebooting."
    echo "  Installation will continue but the login screen will not work"
    echo "  until an X server is installed."
    echo
fi
echo

# ── Build or use prebuilt ─────────────────────────────────────────────────────
if [ -f "xlogin" ] && [ -f "xlogin-launcher" ] && [ -f "pam.d/xlogin" ]; then
    echo "Using prebuilt binaries."
else
    echo "Building from source..."
    apt-get install -y libgtk-3-dev libpam0g-dev build-essential gcc make
    make clean && make
    echo "  build complete."
fi
echo

# ── Install binaries and config ───────────────────────────────────────────────
echo "Installing binaries and config..."
install -m 755 xlogin         /usr/local/bin/
install -m 755 xlogin-launcher /usr/local/bin/
install -m 644 pam.d/xlogin   /etc/pam.d/
install -m 755 etc_init.d_xlogin-launcher /etc/init.d/xlogin-launcher
echo "  done."
echo

# ── Enable seatd at boot ──────────────────────────────────────────────────────
echo "Enabling seatd service..."
update-rc.d seatd defaults
echo "  done."
echo

# ── Target user ───────────────────────────────────────────────────────────────
read -p "Username to configure for graphical login: " TARGET_USER
if ! id "$TARGET_USER" > /dev/null 2>&1; then
    echo "ERROR: user '$TARGET_USER' does not exist." >&2
    exit 1
fi
USER_HOME=$(getent passwd "$TARGET_USER" | cut -d: -f6)

echo "Adding $TARGET_USER to input and video groups..."
usermod -aG input,video "$TARGET_USER"
echo "  done."
echo

# ── Detect installed window managers ─────────────────────────────────────────
echo "Detecting installed window managers..."

WM_LIST=""
WM_CMDS=""

check_wm() {
    local name="$1"
    local cmd="$2"
    if command -v "$cmd" > /dev/null 2>&1; then
        WM_LIST="${WM_LIST}${name} "
        WM_CMDS="${WM_CMDS}${cmd} "
    fi
}

check_wm "jwm"    "jwm"
check_wm "openbox" "openbox-session"
check_wm "xfce4"  "startxfce4"
check_wm "mate"   "mate-session"

CHOSEN_CMD=""

if [ -n "$WM_LIST" ]; then
    # Build numbered menu from detected WMs
    echo "  Detected: $WM_LIST"
    echo
    echo "Which window manager would you like to use?"
    i=1
    for name in $WM_LIST; do
        echo "  $i) $name"
        i=$((i + 1))
    done
    echo
    COUNT=$((i - 1))
    while true; do
        read -p "Enter number [1-$COUNT]: " CHOICE
        if echo "$CHOICE" | grep -qE "^[0-9]+$" && \
           [ "$CHOICE" -ge 1 ] && [ "$CHOICE" -le "$COUNT" ]; then
            break
        fi
        echo "  Invalid choice, please enter a number between 1 and $COUNT."
    done

    # Extract the chosen command
    i=1
    for cmd in $WM_CMDS; do
        if [ "$i" -eq "$CHOICE" ]; then
            CHOSEN_CMD="$cmd"
            break
        fi
        i=$((i + 1))
    done
else
    # No WM found — offer to install one
    echo "  No supported window manager detected."
    echo
    echo "Which would you like to install?"
    echo "  1) jwm"
    echo "  2) openbox"
    echo "  3) xfce4"
    echo "  4) mate"
    echo
    while true; do
        read -p "Enter number [1-4]: " CHOICE
        case "$CHOICE" in
            1) PKG="jwm";          CHOSEN_CMD="jwm";          break ;;
            2) PKG="openbox";      CHOSEN_CMD="openbox-session"; break ;;
            3) PKG="xfce4";        CHOSEN_CMD="startxfce4";   break ;;
            4) PKG="mate-desktop-environment"; CHOSEN_CMD="mate-session"; break ;;
            *) echo "  Invalid choice." ;;
        esac
    done
    echo "Installing $PKG..."
    apt-get install -y "$PKG"
    echo "  done."
fi

echo "Using session command: $CHOSEN_CMD"
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
exec $CHOSEN_CMD
EOF
    chown "$TARGET_USER:$TARGET_USER" "$XINITRC"
    chmod 755 "$XINITRC"
    echo "  Wrote $XINITRC"
fi

# Also install to /etc/skel so new users get a working .xinitrc
cat > /etc/skel/.xinitrc <<EOF
#!/bin/sh
exec $CHOSEN_CMD
EOF
chmod 755 /etc/skel/.xinitrc
echo "  Wrote /etc/skel/.xinitrc"
echo

# ── Configure inittab ─────────────────────────────────────────────────────────
if ! grep -q xlogin-launcher /etc/inittab 2>/dev/null; then
    echo "Updating /etc/inittab..."
    cp /etc/inittab "/etc/inittab.backup.$(date +%Y%m%d_%H%M%S)"
    sed -i '/^1:[0-9]*:respawn:.*[ag]etty/s/^/#/' /etc/inittab
    echo "1:2345:respawn:/usr/local/bin/xlogin-launcher" >> /etc/inittab
    telinit q
    echo "  done."
else
    echo "inittab already configured."
fi
echo

echo "=== Installation complete ==="
echo
echo "Notes:"
echo "  - Log out or reboot to activate the graphical login screen."
echo "  - Users can customise their session by editing ~/.xinitrc"
echo "  - To add another user, re-run this script or manually:"
echo "      usermod -aG input,video <username>"
echo "      cp /etc/skel/.xinitrc /home/<username>/.xinitrc"
echo "      chown <username>:<username> /home/<username>/.xinitrc"
