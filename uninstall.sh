#!/bin/bash
set -e

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: This script must be run as root" >&2
    exit 1
fi

echo "=== simple-login-gui uninstaller ==="
echo

# ── Remove binaries and config ─────────────────────────────────────────────────
echo "Removing binaries and config..."
rm -f /usr/local/bin/xlogin
rm -f /usr/local/bin/xlogin-launcher
rm -f /etc/pam.d/xlogin
rm -f /etc/xlogin.conf
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
echo "  - Users remain in the input and video groups."
echo "    To remove: gpasswd -d <username> input && gpasswd -d <username> video"
