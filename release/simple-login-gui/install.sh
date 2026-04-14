#!/bin/bash
set -e

if [ "$(id -u)" != "0" ]; then
   echo "ERROR: This script must be run as root" 1>&2
   exit 1
fi

echo "Starting simple-login-gui installation..."

# Install runtime dependencies
echo "Installing runtime dependencies..."
if apt-get update && apt-get install -y libgtk-3-0 libpam0g seatd libseat1; then
    echo "✓ Runtime dependencies installed successfully"
else
    echo "✗ Failed to install runtime dependencies"
    exit 1
fi

# Check if prebuilt binaries are available in current directory
if [ -f "xlogin" ] && [ -f "xlogin-launcher" ] && [ -f "pam.d/xlogin" ]; then
    echo "Using prebuilt binaries from current directory"
    if chmod 755 xlogin xlogin-launcher; then
        echo "✓ Prebuilt binaries prepared successfully"
    else
        echo "✗ Failed to set permissions on prebuilt binaries"
        exit 1
    fi
else
    echo "Prebuilt binaries not found, building from source..."
    # Install build dependencies
    if apt-get install -y libgtk-3-dev libpam0g-dev build-essential make gcc; then
        echo "✓ Build dependencies installed successfully"
    else
        echo "✗ Failed to install build dependencies"
        exit 1
    fi

    if make clean && make; then
        echo "✓ Source code compiled successfully"
    else
        echo "✗ Failed to compile source code"
        exit 1
    fi
fi

# Install binaries
echo "Installing binaries..."
if install -m 4750 xlogin /usr/local/bin/ && install -m 755 xlogin-launcher /usr/local/bin/; then
    echo "✓ Binaries installed successfully"
else
    echo "✗ Failed to install binaries"
    exit 1
fi

# Install PAM config
if install -m 644 pam.d/xlogin /etc/pam.d/; then
    echo "✓ PAM configuration installed successfully"
else
    echo "✗ Failed to install PAM configuration"
    exit 1
fi

# Install init.d script
if install -m 755 etc_init.d_xlogin-launcher /etc/init.d/xlogin-launcher; then
    echo "✓ Init.d script installed successfully"
else
    echo "✗ Failed to install init.d script"
    exit 1
fi

# Configure inittab
if ! grep -q xlogin-launcher /etc/inittab; then
    echo "Updating /etc/inittab..."
    # Backup inittab
    cp /etc/inittab /etc/inittab.backup.$(date +%Y%m%d_%H%M%S)
    if sed -i 's/^1:2345:respawn:\/sbin\/getty/#1:2345:respawn:\/sbin\/getty/' /etc/inittab && \
       echo "1:2345:respawn:/usr/local/bin/xlogin-launcher" >> /etc/inittab; then
        echo "✓ inittab updated successfully"
    else
        echo "✗ Failed to update inittab"
        exit 1
    fi
else
    echo "✓ inittab already configured (xlogin-launcher entry found)"
fi

# Reload inittab
echo "Reloading inittab..."
if telinit q; then
    echo "✓ Inittab reloaded successfully"
else
    echo "✗ Failed to reload inittab"
    exit 1
fi

echo ""
echo "Installation complete!"
echo "Please log out to activate the login manager."