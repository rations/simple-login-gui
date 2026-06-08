#!/bin/bash
set -e

VERSION="$1"
if [ -z "$VERSION" ]; then
    echo "Usage: bash make-release.sh <version>" >&2
    echo "  e.g. bash make-release.sh 1.2.0" >&2
    exit 1
fi

RELEASE_NAME="simple-login-gui-${VERSION}"
TARBALL="${RELEASE_NAME}.tar.gz"

echo "=== simple-login-gui release ${VERSION} ==="
echo

# Verify prebuilt binaries exist
for BIN in xlogin-gtk3 xlogin-gtk2; do
    if [ ! -f "$BIN" ]; then
        echo "ERROR: $BIN not found. Run 'make both' first." >&2
        exit 1
    fi
done

# Assemble release directory
echo "Assembling release..."
rm -rf "$RELEASE_NAME"
mkdir "$RELEASE_NAME"
mkdir "$RELEASE_NAME/pam.d"
mkdir "$RELEASE_NAME/polkit"
mkdir "$RELEASE_NAME/src"

cp xlogin-gtk3              "$RELEASE_NAME/"
cp xlogin-gtk2              "$RELEASE_NAME/"
cp xlogin-launcher          "$RELEASE_NAME/"
cp install.sh               "$RELEASE_NAME/"
cp uninstall.sh             "$RELEASE_NAME/"
cp Makefile                 "$RELEASE_NAME/"
cp src/main.c               "$RELEASE_NAME/src/"
cp pam.d/xlogin             "$RELEASE_NAME/pam.d/"
cp polkit/10-local.rules    "$RELEASE_NAME/polkit/"
cp etc_init.d_xlogin-launcher "$RELEASE_NAME/"
cp README.md                "$RELEASE_NAME/"
cp LICENSE                  "$RELEASE_NAME/"

chmod +x "$RELEASE_NAME/install.sh"
chmod +x "$RELEASE_NAME/uninstall.sh"
echo "  done."
echo

# Create tarball
echo "Creating ${TARBALL}..."
tar -czf "$TARBALL" "$RELEASE_NAME/"
rm -rf "$RELEASE_NAME"
echo "  done."
echo

echo "=== Release ready ==="
ls -lh "$TARBALL"
