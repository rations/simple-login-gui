#!/bin/sh
# Build the release binary tarball.
#
# WHAT THIS SHIPS. The binary, the launcher, the PAM stack, the init script, the polkit rules
# and the two fonts the window is lettered in -- the installed surface and nothing else. No
# sources, no build system, no tests, no development tooling. The recipient unpacks it, runs
# the installer that sits next to the tree, and reboots.
#
# THE PREFIX IS PINNED TO /usr/local AND IS NOT AN OPTION. XLOGIN_RESOURCE_DIR_DEFAULT,
# XLOGIN_BACKGROUND_DIR and XLOGIN_CONFIG_PATH are compile-time constants inside the binary, so
# a relocatable archive -- one unpacked wherever the user likes -- would produce a login screen
# that cannot find its fonts or its backgrounds. /usr/local rather than /usr because that is
# where this project has always installed, where xlogin-launcher, the init script and the
# inittab line all point, and because /usr belongs to the distribution's package manager.
#
# HOW IT IS INSTALLED:
#
#     tar -xf xlogin-<version>-<arch>.tar.gz
#     cd xlogin-<version>-<arch>
#     sudo ./install.sh
#
# install.sh is the same file that lives at the top of the source tree; it detects that a usr/
# tree is next to it and copies rather than builds. One file rather than two, so the installer
# that ships is the installer that gets exercised during development.
#
# WHAT A BINARY TARBALL COSTS, STATED RATHER THAN DISCOVERED, and it costs more here than in a
# normal program. The binary is dynamically linked and carries the glibc symbol versions of the
# machine that built it. THIS ONE REPLACES THE tty1 GETTY. If it will not start on the
# recipient's machine and they have already changed /etc/inittab, they have no login screen and
# no text console to read the error on. Two things follow, and both are load-bearing:
#   * the gates below MEASURE the minimum glibc and the exact set of shared libraries, and
#     print them, so the publisher can put the number next to the download; and
#   * install.sh runs the binary and refuses to touch /etc/inittab if it will not start.
# The archive is arch-specific and its name says so.
#
# ONE ARCHIVE: xlogin-<version>-<arch>.tar.gz, plus its checksum.
#
# WHY ustar. Written in the POSIX.1-1988 ustar format, which every tar reads -- bsdtar, busybox,
# toybox, Python tarfile, 7-Zip. pax carries extended headers that smaller tars either ignore or
# spill into the tree as stray PaxHeaders files. ustar's limits (255-byte paths, 8 GiB files) are
# nowhere near binding here, and the check below asserts that rather than assuming it.
#
# REPRODUCIBILITY is bounded. Everything this script controls is normalised -- member order,
# owners, modes, mtimes, the gzip header -- so the archive is a function of the files in it.
# Whether the same tree yields the same BINARY is a property of the toolchain and is not claimed.
#
# This script is POSIX sh but is a MAINTAINER tool: it uses GNU tar, findutils, coreutils and
# binutils. Portability is a property the ARCHIVE must have, because it is unpacked on machines
# nobody here controls; the script only ever runs on the machine cutting the release.

set -eu

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '  %s\n' "$*"; }

VERSION="${1:-}"
[ -n "$VERSION" ] || die "usage: sh make-release.sh <version>   e.g. sh make-release.sh 2.0.0"

root=$(cd "$(dirname "$0")" && pwd)
cd "$root"

for tool in tar gzip sha256sum objdump find sed awk git; do
    command -v "$tool" >/dev/null 2>&1 || die "this script needs $tool"
done
tar --version 2>/dev/null | head -1 | grep -q 'GNU tar' || die "this script needs GNU tar"

arch=$(uname -m)
name="xlogin-${VERSION}-${arch}"
outdir="$root/dist"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

printf '\nBuilding release archive %s\n\n' "$name"

# ---------------------------------------------------------------------------------------------
# Build, at the pinned prefix and with the version compiled in.
# ---------------------------------------------------------------------------------------------
printf 'Building\n'
make clean >/dev/null 2>&1 || true
make VERSION="$VERSION" PREFIX=/usr/local >"$stage/build.log" 2>&1 ||
    { tail -30 "$stage/build.log" >&2; die "the build failed (full log: $stage/build.log)"; }
grep -qE 'warning:' "$stage/build.log" &&
    { grep -nE 'warning:' "$stage/build.log" >&2; die "the build emitted warnings"; }
say "built clean at -Werror"

# The layout audit. A clipped status line is the message telling somebody why they cannot log
# in, and this is the only thing that catches it without rebooting into the login screen.
./tools/uirender resources "$stage" >/dev/null || die "the layout audit failed (tools/uirender)"
say "layout audit passed"

# ---------------------------------------------------------------------------------------------
# Stage. DESTDIR gives the exact absolute tree the recipient receives, built unprivileged.
# ---------------------------------------------------------------------------------------------
printf 'Staging\n'
mkdir "$stage/$name"
make VERSION="$VERSION" PREFIX=/usr/local DESTDIR="$stage/$name" install \
    >"$stage/install.log" 2>&1 ||
    { tail -30 "$stage/install.log" >&2; die "the install failed (log: $stage/install.log)"; }

# What rides at the TOP of the archive rather than being installed: the documents, and the
# installer and uninstaller. install.sh finds the usr/ tree relative to its own location.
for f in README.md LICENSE NOTICE install.sh uninstall.sh; do
    [ -e "$root/$f" ] || die "missing from the tree: $f"
    cp -p "$root/$f" "$stage/$name/$f"
done
say "$(find "$stage/$name" -type f | wc -l) files"

# ---------------------------------------------------------------------------------------------
# Gates. Each one is a thing that has to be true of the ARCHIVE, asserted rather than assumed.
# ---------------------------------------------------------------------------------------------
printf 'Checking the staged tree\n'

# 1. The manifest, EXACTLY. Fail-closed both ways: it fails on a file that appears as well as
#    one that goes missing. An install rule added to the Makefile does not reach a release until
#    it is named here. The paths are literal because the prefix is pinned; that is why it is.
manifest='
install.sh
uninstall.sh
README.md
LICENSE
NOTICE
etc/init.d/xlogin-launcher
etc/pam.d/xlogin
etc/polkit-1/rules.d/10-local.rules
usr/local/bin/xlogin
usr/local/bin/xlogin-launcher
usr/local/share/xlogin/NOTICE
usr/local/share/xlogin/fonts/Michroma-OFL.txt
usr/local/share/xlogin/fonts/Michroma-Regular.ttf
usr/local/share/xlogin/fonts/Roboto-LICENSE.txt
usr/local/share/xlogin/fonts/Roboto-Regular.ttf
'
printf '%s\n' "$manifest" | grep -v '^$' | sort > "$stage/expected"
( cd "$stage/$name" && find . -type f | sed 's|^\./||' | sort ) > "$stage/actual"
if ! diff -u "$stage/expected" "$stage/actual" > "$stage/manifest.diff" 2>&1; then
    printf '%s\n' "the installed tree is not the manifest (- expected, + present):" >&2
    sed -n '3,$p' "$stage/manifest.diff" >&2
    die "update the manifest in this script, or the install rules in the Makefile"
fi
say "the installed tree is exactly the manifest ($(wc -l < "$stage/actual") files)"

# The backgrounds directory must be in the archive and must be EMPTY -- it is where the
# recipient's own images go, and shipping one would be shipping a picture nobody asked for.
[ -d "$stage/$name/usr/local/share/xlogin/backgrounds" ] ||
    die "the backgrounds directory is missing from the archive"
say "backgrounds/ present and empty"

# 2. Nothing local-only escaped -- in the TEXT files and in the BINARY alike. Excluding a
#    development document is easy; shipping a binary with the build machine's home directory
#    baked into a path is the mistake that actually happens, and a text-only grep cannot see it.
#    The ignored names are asked of git rather than written here, because writing them out would
#    mean this script named them, which is the thing being forbidden.
ignored=$(git ls-files --others --ignored --exclude-standard --directory 2>/dev/null |
          sed 's:/*$::' | xargs -r -n1 basename | sort -u |
          grep -vE '^(xlogin|uirender|dist|.*\.(o|d|so|a|log|tar\.gz|sha256))$' |
          grep -E '^.{4,}$' || true)
pattern='/home/[A-Za-z0-9._-]'
for n in $ignored; do
    pattern="$pattern|$(printf '%s' "$n" | sed 's/[.[\*^$]/\\&/g')\\b"
done
leaks=$(grep -rlE --binary-files=text "$pattern" "$stage/$name" 2>/dev/null || true)
[ -z "$leaks" ] || {
    printf '%s\n' "$leaks" >&2
    grep -rnoE --binary-files=text "$pattern" "$stage/$name" 2>/dev/null | head -10 >&2
    die "a file in the archive names a local-only path or a document the recipient has not got"
}
say "no local-only paths, in text or in the binary"

# 3. No symlinks. An archive with none cannot have a link-escape problem at unpack time, and
#    this one is unpacked as root over /.
links=$(find "$stage/$name" -type l)
[ -z "$links" ] || { printf '%s\n' "$links" >&2; die "the archive contains symlinks"; }
say "no symlinks"

# 4. ustar's limits are not binding. Asserted rather than assumed, because the failure mode is
#    a truncated path on the recipient's machine rather than an error here.
long=$(cd "$stage/$name" && find . -printf '%P\n' | awk 'length($0) > 99 { print }')
[ -z "$long" ] || { printf '%s\n' "$long" >&2; die "path too long for ustar (>99 bytes)"; }
say "every path fits ustar"

# ---------------------------------------------------------------------------------------------
# 5. What the binary needs from the recipient's machine. The question a source tarball never had
#    to answer and a binary one cannot avoid.
# ---------------------------------------------------------------------------------------------
printf 'Measuring what the binary needs\n'

gui="$stage/$name/usr/local/bin/xlogin"
needed() { objdump -p "$1" | awk '/NEEDED/ { print $2 }'; }

# An allowlist, for the same reason the manifest is one: a new NEEDED entry is a new thing the
# recipient has to already have, and it should be decided rather than discovered.
allowed='libcairo.so.2 libfreetype.so.6 libjpeg.so.62 libX11.so.6 libXrandr.so.2
         libpam.so.0 libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6
         ld-linux-x86-64.so.2 ld-linux-aarch64.so.1 ld-linux-armhf.so.3'
for lib in $(needed "$gui"); do
    case " $(echo $allowed) " in
        *" $lib "*) ;;
        *) die "xlogin links a library that is not on the allowlist: $lib
Either it belongs in the release's dependency list -- add it above, to the README and to the
apt-get line in install.sh -- or it was linked by accident." ;;
    esac
done
say "links: $(needed "$gui" | tr '\n' ' ')"

glibc=$(objdump -p "$gui" | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' |
        sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
[ -n "$glibc" ] || die "could not read the glibc symbol versions out of the binary"
cxx=$(objdump -p "$gui" | sed -n 's/.*\(GLIBCXX_[0-9][0-9.]*\).*/\1/p' |
      sort -t. -k2,2n -k3,3n -k4,4n | tail -1)
say "needs glibc >= $glibc and $cxx on $arch  <-- PUBLISH THIS NEXT TO THE DOWNLOAD"

# The hardening the README claims, asserted on the SHIPPED binary rather than on a build.
readelf -hlWd "$gui" > "$stage/elf" 2>/dev/null || die "readelf could not read the binary"
grep -q 'GNU_RELRO' "$stage/elf"        || die "the shipped binary has no RELRO"
grep -q 'BIND_NOW'  "$stage/elf"        || die "the shipped binary is not BIND_NOW"
grep -qE 'GNU_STACK.+RW ' "$stage/elf"  || die "the shipped binary's stack is not NX"
grep -q 'DYN (Position-Independent Executable' "$stage/elf" ||
    die "the shipped binary is not PIE"
say "PIE, full RELRO, BIND_NOW and NX asserted on the shipped binary"

# ---------------------------------------------------------------------------------------------
# 6. Normalise, so the archive is a function of the files in it.
# ---------------------------------------------------------------------------------------------
find "$stage/$name" -type d -exec chmod 755 {} +
find "$stage/$name" -type f -exec chmod 644 {} +
chmod 755 "$gui" "$stage/$name/usr/local/bin/xlogin-launcher" \
          "$stage/$name/etc/init.d/xlogin-launcher" \
          "$stage/$name/install.sh" "$stage/$name/uninstall.sh"

if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    epoch=$SOURCE_DATE_EPOCH
else
    # The commit, not the clock: the binary has no meaningful mtime of its own -- it was made a
    # minute ago -- so the sources it was made from are the only honest timestamp.
    epoch=$(git log -1 --format=%ct 2>/dev/null || true)
    [ -n "${epoch:-}" ] || epoch=$(date +%s)
fi
find "$stage/$name" -exec touch -d "@$epoch" {} +
say "mtime pinned to $(date -u -d "@$epoch" '+%Y-%m-%d %H:%M:%S UTC')"

# ---------------------------------------------------------------------------------------------
# 7. Pack.
# ---------------------------------------------------------------------------------------------
printf 'Packing\n'
mkdir -p "$outdir"
( cd "$stage" && tar --format=ustar --sort=name --numeric-owner --owner=0 --group=0 \
      --mtime="@$epoch" -cf "$stage/$name.tar" "$name" )
gzip -9nc "$stage/$name.tar" > "$outdir/$name.tar.gz"
( cd "$outdir" && sha256sum "$name.tar.gz" > "$name.sha256" )
say "$outdir/$name.tar.gz ($(du -h "$outdir/$name.tar.gz" | cut -f1))"
say "$outdir/$name.sha256"

# ---------------------------------------------------------------------------------------------
# 8. Unpack what was just written and check THAT, not the staging directory. This is the copy
#    the recipient gets; they differ exactly when packing goes wrong.
# ---------------------------------------------------------------------------------------------
printf 'Checking the packed archive\n'
check="$stage/check"
mkdir "$check"
tar -C "$check" -xf "$outdir/$name.tar.gz"

( cd "$check/$name" && find . -type f | sed 's|^\./||' | sort ) > "$stage/repacked"
diff -q "$stage/expected" "$stage/repacked" >/dev/null ||
    die "the packed archive is not the manifest"
say "the unpacked archive is exactly the manifest"

# THE BINARY RUNS. An archive whose binary has never been executed is one nobody has checked,
# and for this program a loader error lands on a machine with no login screen and no getty.
if ! out=$("$check/$name/usr/local/bin/xlogin" --version 2>&1); then
    printf '%s\n' "$out" >&2
    die "the shipped binary does not run"
fi
[ "$out" = "xlogin $VERSION" ] ||
    die "the shipped binary reports '$out', not 'xlogin $VERSION' -- it is a stale build"
say "the shipped binary runs and reports: $out"

# And it is the paths the archive was built for, read back out of the binary rather than
# assumed. A binary built for a different prefix unpacks fine and then cannot find its fonts.
"$check/$name/usr/local/bin/xlogin" --help | grep -q '/usr/local/share/xlogin/fonts' ||
    die "the shipped binary was not built for /usr/local -- it will not find its fonts"
say "the shipped binary's compiled-in paths match the archive layout"

# install.sh must recognise the archive as a binary archive, not try to build in it.
[ -f "$check/$name/Makefile" ] &&
    die "a Makefile reached the archive; install.sh would try to build"
grep -q 'MODE=binary' "$check/$name/install.sh" ||
    die "the shipped install.sh has no binary mode"
say "install.sh will take the binary path"

printf '\n=== Release ready ===\n\n'
ls -l "$outdir/$name.tar.gz" "$outdir/$name.sha256"
cat <<EOF

Publish alongside it:
    architecture   $arch
    requires       glibc >= $glibc, $cxx
    shared libs    $(needed "$gui" | tr '\n' ' ')

Install with:
    tar -xf $name.tar.gz
    cd $name
    sudo ./install.sh
EOF
