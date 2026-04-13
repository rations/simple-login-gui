#!/bin/sh
set -e

if [ "$(id -u)" != "0" ]; then
   echo "This script must be run as root" 1>&2
   exit 1
fi

echo "Installing simple-login-gui..."

# Install dependencies
apt-get update
apt-get install -y libgtk-3-0 libpam0g seatd libseat1

# Install binaries
install -m 755 xlogin /usr/local/bin/
install -m 755 xlogin-launcher /usr/local/bin/
install -m 644 pam.d/xlogin /etc/pam.d/

# Configure inittab
if ! grep -q xlogin-launcher /etc/inittab; then
    echo "Updating /etc/inittab..."
    sed -i 's/^1:2345:respawn:\/sbin\/getty/#1:2345:respawn:\/sbin\/getty/' /etc/inittab
    echo "1:2345:respawn:/usr/local/bin/xlogin-launcher" >> /etc/inittab
fi

adduser root video

echo "Installation complete. Reboot to activate login manager."
