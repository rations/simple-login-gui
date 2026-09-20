#!/bin/bash
set -e

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: This script must be run as root" >&2
    exit 1
fi

echo "=== simple-login-gui uninstaller ==="
echo

# ── Remove binaries and config ─────────────────────────────────────────────────
echo "Removing binaries, fonts and config..."
rm -f /usr/local/bin/xlogin
rm -f /usr/local/bin/xlogin-launcher
rm -f /etc/pam.d/xlogin
rm -f /etc/polkit-1/rules.d/10-local.rules

# /etc/xlogin.conf is backed up before it goes. XSERVER_FLAGS in it may have taken
# somebody an afternoon to work out for their GPU, and "I uninstalled it to try
# something else" is not a reason to make them do that again.
if [ -f /etc/xlogin.conf ]; then
    CONF_BACKUP="/etc/xlogin.conf.backup.$(date +%Y%m%d_%H%M%S)"
    cp /etc/xlogin.conf "$CONF_BACKUP"
    rm -f /etc/xlogin.conf
    echo "  /etc/xlogin.conf removed (backed up to $CONF_BACKUP)"
fi

# The bundled fonts, the NOTICE, and any background images that were installed.
# Asked about, because an image the user put there is theirs and not ours.
if [ -d /usr/local/share/xlogin ]; then
    BG_COUNT=$(find /usr/local/share/xlogin/backgrounds -type f 2>/dev/null | wc -l)
    if [ "$BG_COUNT" -gt 0 ]; then
        echo
        echo "  /usr/local/share/xlogin/backgrounds holds $BG_COUNT image(s)."
        read -p "  Delete them along with the bundled fonts? [y/N]: " DEL_BG
        case "$DEL_BG" in
            [yY]*)
                rm -rf /usr/local/share/xlogin
                echo "  Removed /usr/local/share/xlogin"
                ;;
            *)
                rm -rf /usr/local/share/xlogin/fonts
                rm -f  /usr/local/share/xlogin/NOTICE
                echo "  Kept /usr/local/share/xlogin/backgrounds; removed the fonts."
                ;;
        esac
    else
        rm -rf /usr/local/share/xlogin
    fi
fi
echo "  done."
echo

# ── Remove and disable init.d service ─────────────────────────────────────────
echo "Removing xlogin-launcher service..."
if [ -f /etc/init.d/xlogin-launcher ]; then
    LC_ALL=C update-rc.d xlogin-launcher remove
    rm -f /etc/init.d/xlogin-launcher
    echo "  done."
else
    echo "  /etc/init.d/xlogin-launcher not found, skipping."
fi
echo

# ── Restore /etc/inittab ───────────────────────────────────────────────────────
if grep -q xlogin-launcher /etc/inittab 2>/dev/null; then
    echo "Restoring /etc/inittab..."
    cp /etc/inittab "/etc/inittab.backup.$(date +%Y%m%d_%H%M%S)"
    sed -i '/xlogin-launcher/d' /etc/inittab
    sed -i '/^#1:[0-9]*:respawn:.*[ag]etty/s/^#//' /etc/inittab
    telinit q
    echo "  done."
else
    echo "  inittab: xlogin-launcher not found, no changes needed."
fi
echo

echo "=== Uninstall complete ==="
echo
echo "Notes:"
echo "  - Reboot to return to the text console login."
echo "  - User ~/.xinitrc files were not removed."
echo "  - /etc/skel/.xinitrc was not removed."
echo "  - /etc/xlogin.conf was backed up beside itself before removal."
echo "  - Users remain in the input, video, and plugdev groups."
echo "    To remove: gpasswd -d <username> input"
echo "               gpasswd -d <username> video"
echo "               gpasswd -d <username> plugdev"
