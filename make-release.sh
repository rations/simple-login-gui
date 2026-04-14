#!/bin/sh
set -e

echo "Building release..."

make clean
make

mkdir -p release
cp xlogin release/
cp xlogin-launcher release/
cp -r pam.d release/
cp install.sh release/
chmod +x release/install.sh
cp Makefile release/
cp README.md release/
sed -i 's/Download the latest release tarball from \[GitHub Releases\](https:\/\/github.com\/rations\/simple-login-gui\/releases):/Since you already have the release tarball extracted:/' release/README.md
sed -i '/wget https:\/\/github.com\/rations\/simple-login-gui\/releases\/latest\/download\/simple-login-gui.tar.gz/d' release/README.md
sed -i '/tar -xzf simple-login-gui.tar.gz/d' release/README.md
sed -i '/cd simple-login-gui/d' release/README.md
cp LICENSE release/

cd release
tar -czf ../simple-login-gui.tar.gz .
cd ..

sha256sum simple-login-gui.tar.gz > simple-login-gui.sha256

echo ""
echo "Release created:"
ls -lh simple-login-gui.tar.gz simple-login-gui.sha256
echo ""
echo "Now upload simple-login-gui.tar.gz to GitHub Releases. This will be automatically picked up by the install script."
