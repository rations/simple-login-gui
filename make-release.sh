#!/bin/bash
set -e

echo "Building simple-login-gui release..."

# Clean previous build
echo "Cleaning previous build..."
if make clean; then
    echo "✓ Cleaned successfully"
else
    echo "✗ Failed to clean"
    exit 1
fi

# Build the project
echo "Building project..."
if make; then
    echo "✓ Build successful"
else
    echo "✗ Build failed"
    exit 1
fi

# Prepare release directory
echo "Preparing release directory..."
rm -rf release
mkdir -p release/simple-login-gui

# Copy essential files for installation
echo "Copying files to release..."
cp xlogin release/simple-login-gui/
cp xlogin-launcher release/simple-login-gui/
cp -r pam.d release/simple-login-gui/
cp etc_init.d_xlogin-launcher release/simple-login-gui/
cp install.sh release/simple-login-gui/
chmod +x release/simple-login-gui/install.sh
cp Makefile release/simple-login-gui/
cp README.md release/simple-login-gui/
cp LICENSE release/simple-login-gui/
echo "✓ Files copied successfully"

# Create tarball
echo "Creating tarball..."
cd release
if tar -czf ../simple-login-gui.tar.gz simple-login-gui/; then
    cd ..
    echo "✓ Tarball created"
else
    cd ..
    echo "✗ Failed to create tarball"
    exit 1
fi

# Generate checksum
echo "Generating checksum..."
if sha256sum simple-login-gui.tar.gz > simple-login-gui.sha256; then
    echo "✓ Checksum generated"
else
    echo "✗ Failed to generate checksum"
    exit 1
fi

echo ""
echo "🎉 Release created successfully!"
echo "Files:"
ls -lh simple-login-gui.tar.gz simple-login-gui.sha256
echo ""
echo "Upload simple-login-gui.tar.gz to GitHub Releases for automatic installation."
