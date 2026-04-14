#!/bin/sh
set -e

echo "Building release..."

make clean
make

mkdir -p release/simple-login-gui
cp xlogin release/simple-login-gui/
cp xlogin-launcher release/simple-login-gui/
cp -r pam.d release/simple-login-gui/
cp install.sh release/simple-login-gui/
chmod +x release/simple-login-gui/install.sh
cp Makefile release/simple-login-gui/
cp README.md release/simple-login-gui/
sed -i 's/Download the latest release tarball from \[GitHub Releases\](https:\/\/github.com\/rations\/simple-login-gui\/releases):/Since you already have the release tarball extracted:/' release/simple-login-gui/README.md
sed -i '/wget https:\/\/github.com\/rations\/simple-login-gui\/releases\/latest\/download\/simple-login-gui.tar.gz/d' release/simple-login-gui/README.md
sed -i '/tar -xzf simple-login-gui.tar.gz/d' release/simple-login-gui/README.md
sed -i '/cd simple-login-gui/d' release/simple-login-gui/README.md
cp LICENSE release/simple-login-gui/

cd release
tar -czf ../simple-login-gui.tar.gz simple-login-gui/
cd ..

sha256sum simple-login-gui.tar.gz > simple-login-gui.sha256

echo ""
echo "Release created:"
ls -lh simple-login-gui.tar.gz simple-login-gui.sha256
echo ""
echo "Now upload simple-login-gui.tar.gz to GitHub Releases. This will be automatically picked up by the install script."
