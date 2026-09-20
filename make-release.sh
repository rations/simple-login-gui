#!/bin/bash
set -e

VERSION="$1"
if [ -z "$VERSION" ]; then
    echo "Usage: bash make-release.sh <version>" >&2
    echo "  e.g. bash make-release.sh 2.0.0" >&2
    exit 1
fi

RELEASE_NAME="simple-login-gui-${VERSION}"
TARBALL="${RELEASE_NAME}.tar.gz"

echo "=== simple-login-gui release ${VERSION} ==="
echo

# ── The release must build ────────────────────────────────────────────────────
# Source-only now, so a tarball that does not compile is a tarball nobody can
# install. Built here rather than trusted.
echo "Building from a clean tree..."
make clean > /dev/null
make > /dev/null
echo "  builds clean at -Werror."

# ── The layout must fit ───────────────────────────────────────────────────────
# tools/uirender renders the real panel with the real fonts and fails if any
# string overflows its slot or any two rows of ink overlap. A clipped status
# line is the error message that tells somebody why they cannot log in.
echo "Auditing the layout..."
AUDIT_DIR=$(mktemp -d)
if ! ./tools/uirender resources "$AUDIT_DIR" > /dev/null; then
    rm -rf "$AUDIT_DIR"
    echo "ERROR: the layout audit failed. Run tools/uirender to see why." >&2
    exit 1
fi
rm -rf "$AUDIT_DIR"
echo "  layout audit passed."

# ── Nothing local may ship ────────────────────────────────────────────────────
# This repository has development notes that git ignores, and a release tarball is
# the one place a stale reference to one would reach somebody who has no way to
# read it. So: no tracked file may name an ignored path, and none may name an
# absolute path from the machine this was built on.
#
# The list of ignored names is ASKED OF GIT rather than written out here. Writing
# them out would mean this script itself named them, which is the thing being
# forbidden -- and it would go stale the first time somebody added another one.
# Any leftover release directory from a previous run is itself git-ignored, so it would
# join the list of names below and be searched for. Clear it first.
rm -rf "$RELEASE_NAME"

echo "Sweeping for local-only references..."
# Build artifacts are ignored too and are not what this is looking for, so they are
# dropped -- as is any name shorter than four characters, which would be too generic
# to search a source tree for without matching something innocent.
IGNORED_NAMES=$(git ls-files --others --ignored --exclude-standard --directory |
                sed 's:/*$::' | xargs -r -n1 basename | sort -u |
                grep -vE '^(xlogin|uirender|.*\.(o|d|so|a|tar\.gz))$' |
                grep -E '^.{4,}$' || true)

# Each name is anchored with a trailing word boundary. Without it the match is a bare
# substring and a generated name like `panel.d` matches `panel.draw(c)` in ordinary source
# -- which it did, the first time the dependency files existed.
PATTERN='/home/[A-Za-z0-9._-]'
for NAME in $IGNORED_NAMES; do
    PATTERN="$PATTERN|$(printf '%s' "$NAME" | sed 's/[.[\*^$]/\\&/g')\\b"
done

LEAKS=$(git ls-files -z | grep -zv '^\.gitignore$' |
        xargs -0 -r grep -nE "$PATTERN" 2>/dev/null || true)
if [ -n "$LEAKS" ]; then
    echo "ERROR: a tracked file names a local-only path, or a file that is not shipped:" >&2
    echo "$LEAKS" >&2
    exit 1
fi
echo "  clean (checked against: $(echo $IGNORED_NAMES | tr '\n' ' '))"

# ── No compiled binary goes in the tarball ────────────────────────────────────
# There used to be two, one per GTK version. Shipping a compiled root-privileged
# login manager to be installed sight-unseen was never worth what it saved.
echo "Assembling release..."
rm -rf "$RELEASE_NAME"
mkdir -p "$RELEASE_NAME"/{pam.d,polkit,src/gfx,src/platform,src/session,src/ui,tools,resources/fonts,docs}

cp Makefile                   "$RELEASE_NAME/"
cp install.sh                 "$RELEASE_NAME/"
cp uninstall.sh               "$RELEASE_NAME/"
cp xlogin-launcher            "$RELEASE_NAME/"
cp etc_init.d_xlogin-launcher "$RELEASE_NAME/"
cp README.md                  "$RELEASE_NAME/"
cp LICENSE                    "$RELEASE_NAME/"
cp NOTICE                     "$RELEASE_NAME/"
cp .clang-format              "$RELEASE_NAME/"

cp src/*.h src/*.c src/*.cpp  "$RELEASE_NAME/src/"
cp src/gfx/*                  "$RELEASE_NAME/src/gfx/"
cp src/platform/*             "$RELEASE_NAME/src/platform/"
cp src/session/*              "$RELEASE_NAME/src/session/"
cp src/ui/*                   "$RELEASE_NAME/src/ui/"
cp tools/uirender.cpp         "$RELEASE_NAME/tools/"
cp pam.d/xlogin               "$RELEASE_NAME/pam.d/"
cp polkit/10-local.rules      "$RELEASE_NAME/polkit/"
cp resources/fonts/*          "$RELEASE_NAME/resources/fonts/"
# README.md shows this; without it the tarball's README has a broken image.
cp docs/screenshot.png        "$RELEASE_NAME/docs/"

chmod +x "$RELEASE_NAME/install.sh" "$RELEASE_NAME/uninstall.sh"

# Build leftovers must not travel. The src/*/* copies above are wildcards, so both the
# objects and the -MMD dependency files land in them.
find "$RELEASE_NAME" -type f \( -name '*.o' -o -name '*.d' \) -delete
echo "  done."
echo

# ── The tarball must build too ────────────────────────────────────────────────
# Because the thing that was tested above is the working tree, and the thing
# somebody downloads is this. They differ exactly when a file was forgotten.
echo "Building the assembled tree, to prove nothing was left out..."
if ! ( cd "$RELEASE_NAME" && make > /dev/null 2>&1 ); then
    echo "ERROR: the assembled release does not build. A file is missing from" >&2
    echo "       the copy list above." >&2
    exit 1
fi
( cd "$RELEASE_NAME" && make clean > /dev/null 2>&1 )
find "$RELEASE_NAME" -type f \( -name '*.o' -o -name '*.d' \) -delete
rm -f "$RELEASE_NAME/xlogin" "$RELEASE_NAME/tools/uirender"
echo "  the release tree builds."
echo

echo "Creating ${TARBALL}..."
tar -czf "$TARBALL" "$RELEASE_NAME/"
rm -rf "$RELEASE_NAME"
echo "  done."
echo

echo "=== Release ready ==="
ls -lh "$TARBALL"
