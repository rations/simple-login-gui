#!/bin/sh
set -e

echo "Building release..."

make clean
make

mkdir -p release
cp xlogin release/
cp xlogin-launcher release/
cp pam.d/xlogin release/

cd release
tar -czf ../simple-login-gui.tar.gz .
cd ..

sha256sum simple-login-gui.tar.gz > simple-login-gui.sha256

echo ""
echo "Release created:"
ls -lh simple-login-gui.tar.gz simple-login-gui.sha256
echo ""
echo "Now upload simple-login-gui.tar.gz to GitHub Releases. This will be automatically picked up by the install script."
