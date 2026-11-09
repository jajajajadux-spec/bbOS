#!/usr/bin/env bash
#
# build-bbos-toolchain.sh
#
# Full-manual cross-toolchain + kernel + userland bootstrap for BurntballOS (bbOS).
# Run this on your OWN Linux machine/VM (not a container) with the build deps below.
# Designed to run unattended overnight: `nohup ./build-bbos-toolchain.sh > build.log 2>&1 &`
# then `tail -f build.log` to watch, or just check it in the morning.
#
# Host prerequisites (Debian/Ubuntu):
#   sudo apt update && sudo apt install -y build-essential bison flex texinfo \
#     gawk wget curl python3 rsync xz-utils bzip2 bc libncurses-dev \
#     libelf-dev libssl-dev libgmp-dev libmpfr-dev libmpc-dev libisl-dev file
#
# DO NOT run this as root. Create a dedicated build user first.

set -euo pipefail

if (( EUID == 0 )); then
  echo "Run as an unprivileged build user." >&2
  exit 1
fi

# ---------- CONFIG ----------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export TARGET=x86_64-bbos-linux-gnu
export BB_ROOT="${BB_ROOT:-$HOME/bbos-build}"
export SRC="$BB_ROOT/sources"
export TOOLS="$BB_ROOT/tools"        # cross-toolchain install prefix
export SYSROOT="$BB_ROOT/sysroot"    # target root (glibc, kernel headers live here)
export BUILD="$BB_ROOT/build"
export PATH="$TOOLS/bin:$PATH"
export JOBS="${JOBS:-$(nproc)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid JOBS" >&2; exit 1; }

# Never let a doc-generation tool that isn't installed kill the build
export MAKEINFO=true

# Install dependencies explicitly before starting; never hide failed setup steps.
for tool in gcc g++ make bison flex makeinfo gawk wget curl python3 rsync xz bzip2 bc file; do
  command -v "$tool" >/dev/null || { echo "Missing build prerequisite: $tool" >&2; exit 1; }
done

# Pinned bootstrap versions; see README for validation limits.
BINUTILS_VER=2.42
GCC_VER=13.2.0
GLIBC_VER=2.39
LINUX_VER=6.6.21
BUSYBOX_VER=1.36.1

mkdir -p "$SRC" "$TOOLS" "$SYSROOT" "$BUILD"
cd "$SRC"

log() { echo -e "\n\n===== [$(date '+%H:%M:%S')] $1 =====\n"; }

# ---------- 1. FETCH SOURCES ----------
log "Downloading sources"
fetch() {
  local archive
  archive=$(basename "$1")
  if [[ ! -s "$archive" ]]; then
    wget --https-only -q --show-progress -O "$archive.part" "$1"
    mv "$archive.part" "$archive"
  fi
}

fetch "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
fetch "https://ftp.gnu.org/gnu/glibc/glibc-${GLIBC_VER}.tar.xz"
fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_VER}.tar.xz"
fetch "https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2"

for f in *.tar.xz; do [ -d "${f%.tar.xz}" ] || tar xf "$f"; done
for f in *.tar.bz2; do [ -d "${f%.tar.bz2}" ] || tar xf "$f"; done

# GCC wants its prerequisite libs (gmp/mpfr/mpc/isl) vendored in
cd "$SRC/gcc-${GCC_VER}"
[ -f contrib/download_prerequisites.output ] || {
  contrib/download_prerequisites && touch contrib/download_prerequisites.output
}
cd "$SRC"

# ---------- 2. BINUTILS (cross) ----------
log "Building binutils (cross)"
mkdir -p "$BUILD/binutils" && cd "$BUILD/binutils"
"$SRC/binutils-${BINUTILS_VER}/configure" \
  --prefix="$TOOLS" --target="$TARGET" --with-sysroot="$SYSROOT" \
  --disable-nls --disable-werror
make -j"$JOBS" MAKEINFO=true || make -j1 MAKEINFO=true
make install MAKEINFO=true

# ---------- 3. GCC PASS 1 (no libc, C only) ----------
log "Building GCC pass 1 (bootstrap, no libc)"
mkdir -p "$BUILD/gcc-pass1" && cd "$BUILD/gcc-pass1"
"$SRC/gcc-${GCC_VER}/configure" \
  --prefix="$TOOLS" --target="$TARGET" --with-sysroot="$SYSROOT" \
  --disable-nls --disable-multilib --disable-shared --without-headers --enable-languages=c \
  --disable-threads --disable-libatomic --disable-libgomp --disable-libquadmath \
  --disable-libssp --disable-libvtv --disable-libstdcxx
make -j"$JOBS" MAKEINFO=true all-gcc all-target-libgcc || make -j1 MAKEINFO=true all-gcc all-target-libgcc
make MAKEINFO=true install-gcc install-target-libgcc

# ---------- 4. LINUX KERNEL HEADERS ----------
log "Installing sanitized kernel headers into sysroot"
cd "$SRC/linux-${LINUX_VER}"
make mrproper
make ARCH=x86_64 INSTALL_HDR_PATH="$SYSROOT/usr" headers_install

# ---------- 5. GLIBC ----------
log "Building glibc against sysroot"
mkdir -p "$BUILD/glibc" && cd "$BUILD/glibc"
"$SRC/glibc-${GLIBC_VER}/configure" \
  --prefix=/usr --host="$TARGET" --build="$(gcc -dumpmachine)" \
  --with-headers="$SYSROOT/usr/include" \
  libc_cv_forced_unwind=yes
make -j"$JOBS" MAKEINFO=true || make -j1 MAKEINFO=true
make DESTDIR="$SYSROOT" install MAKEINFO=true

# ---------- 6. GCC PASS 2 (full, linked against real libc) ----------
log "Building GCC pass 2 (full compiler)"
mkdir -p "$BUILD/gcc-pass2" && cd "$BUILD/gcc-pass2"
"$SRC/gcc-${GCC_VER}/configure" \
  --prefix="$TOOLS" --target="$TARGET" --with-sysroot="$SYSROOT" \
  --enable-languages=c,c++ --disable-nls --disable-multilib --disable-bootstrap \
  --enable-shared --enable-threads=posix
make -j"$JOBS" MAKEINFO=true || make -j1 MAKEINFO=true
make install MAKEINFO=true

# ---------- 7. FULL TARGET KERNEL ----------
log "Building the actual bbOS kernel"
cd "$SRC/linux-${LINUX_VER}"
make ARCH=x86_64 defconfig
# Optional: run `make ARCH=x86_64 menuconfig` here interactively before an overnight
# run if you want to trim it down; defconfig is a safe, generic starting point.
make ARCH=x86_64 CROSS_COMPILE="${TARGET}-" -j"$JOBS" bzImage || make ARCH=x86_64 CROSS_COMPILE="${TARGET}-" -j1 bzImage
cp arch/x86/boot/bzImage "$BB_ROOT/bbos-kernel"

# ---------- 8. BUSYBOX (cross-compiled userland) ----------
log "Building BusyBox against the new toolchain"
cd "$SRC/busybox-${BUSYBOX_VER}"
make ARCH=x86_64 CROSS_COMPILE="${TARGET}-" defconfig
sed -i 's/^# CONFIG_STATIC.*/CONFIG_STATIC=y/' .config  # static binary = simplest to boot
make ARCH=x86_64 CROSS_COMPILE="${TARGET}-" -j"$JOBS" || make ARCH=x86_64 CROSS_COMPILE="${TARGET}-" -j1
make ARCH=x86_64 CROSS_COMPILE="${TARGET}-" CONFIG_PREFIX="$SYSROOT" install

# ---------- 9. BBOS USERLAND ----------
make -B -C "$SCRIPT_DIR" CC="${TARGET}-gcc" LDFLAGS=-static all
install -m 0755 "$SCRIPT_DIR/build/init" "$SYSROOT/init"
install -D -m 0755 "$SCRIPT_DIR/build/bbpkg" "$SYSROOT/usr/bin/bbpkg"
mkdir -p "$SYSROOT"/{dev,proc,sys,tmp,var/lib/bbpkg/installed}
chmod 1777 "$SYSROOT/tmp"

log "DONE. Kernel at $BB_ROOT/bbos-kernel, cross toolchain in $TOOLS, sysroot in $SYSROOT"
echo "Next: build a rootfs from \$SYSROOT + BusyBox install, boot it in QEMU (see README.md)."
