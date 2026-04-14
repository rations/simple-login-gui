#!/bin/sh
set -e

if [ "$(id -u)" != "0" ]; then
   echo "This script must be run as root" 1>&2
   exit 1
fi

echo "Installing simple-login-gui..."

# Install runtime dependencies
apt-get update
apt-get install -y libgtk-3-0 libpam0g seatd libseat1

# Check if prebuilt binaries are available in current directory
if [ -f "xlogin" ] && [ -f "xlogin-launcher" ] && [ -f "pam.d/xlogin" ]; then
    echo "Using prebuilt binaries from current directory"
    chmod 755 xlogin xlogin-launcher
else
    echo "Prebuilt binaries not found, building from source..."
    # Install build dependencies
    apt-get install -y libgtk-3-dev libpam0g-dev build-essential make gcc
    make clean
    make
fi

# Install binaries
install -m 755 xlogin /usr/local/bin/
install -m 755 xlogin-launcher /usr/local/bin/
install -m 644 pam.d/xlogin /etc/pam.d/

# Clean up
rm -f xlogin xlogin-launcher
rm -rf pam.d
make clean 2>/dev/null || true

# Configure inittab
if ! grep -q xlogin-launcher /etc/inittab; then
    echo "Updating /etc/inittab..."
    sed -i 's/^1:2345:respawn:\/sbin\/getty/#1:2345:respawn:\/sbin\/getty/' /etc/inittab
    echo "1:2345:respawn:/usr/local/bin/xlogin-launcher" >> /etc/inittab
fi

adduser root video

echo "Installation complete. Reboot to activate login manager."
