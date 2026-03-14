#!/bin/sh

set -eu

BOARD_DEFCONFIG="rk3288_philips_defconfig"
LOCAL_TC_DIR="$(pwd)/toolchain/arm-eabi-4.9"
LOCAL_TC_PREFIX="$LOCAL_TC_DIR/bin/arm-eabi-"
LINARO_URL="https://releases.linaro.org/components/toolchain/binaries/4.9-2017.01/arm-eabi/gcc-linaro-4.9.4-2017.01-x86_64_arm-eabi.tar.xz"
LINARO_ARCHIVE="$(basename "$LINARO_URL")"
LINARO_EXTRACT_DIR="gcc-linaro-4.9.4-2017.01-x86_64_arm-eabi"

nproc_cmd() {
	if command -v nproc >/dev/null 2>&1; then
		nproc
	else
		getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1
	fi
}

compiler_exists() {
	prefix="$1"
	if [ -z "$prefix" ]; then
		return 1
	fi
	command -v "${prefix}gcc" >/dev/null 2>&1
}

prompt_download_toolchain() {
	answer=""
	printf "Download and install the Linaro 4.9 ARM EABI toolchain now? [Y/n] "
	read -r answer || answer="n"
	case "$answer" in
		""|y|Y|yes|YES)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

download_toolchain() {
	mkdir -p toolchain

	if command -v wget >/dev/null 2>&1; then
		wget -O "toolchain/$LINARO_ARCHIVE" "$LINARO_URL"
	elif command -v curl >/dev/null 2>&1; then
		curl -L "$LINARO_URL" -o "toolchain/$LINARO_ARCHIVE"
	else
		echo "Need wget or curl to download the toolchain."
		return 1
	fi

	rm -rf "$LOCAL_TC_DIR"
	tar -C toolchain -xf "toolchain/$LINARO_ARCHIVE"
	mv "toolchain/$LINARO_EXTRACT_DIR" "$LOCAL_TC_DIR"
	rm -f "toolchain/$LINARO_ARCHIVE"

	if [ ! -x "${LOCAL_TC_PREFIX}gcc" ]; then
		echo "Toolchain download completed, but arm-eabi-gcc was not found."
		return 1
	fi

	export CROSS_COMPILE="$LOCAL_TC_PREFIX"
}

ensure_toolchain() {
	if compiler_exists "${CROSS_COMPILE:-}"; then
		return 0
	fi

	if [ -x "${LOCAL_TC_PREFIX}gcc" ]; then
		export CROSS_COMPILE="$LOCAL_TC_PREFIX"
		return 0
	fi

	if command -v arm-eabi-gcc >/dev/null 2>&1; then
		export CROSS_COMPILE="arm-eabi-"
		return 0
	fi

	if prompt_download_toolchain; then
		download_toolchain
		return 0
	fi

	cat <<EOF
Missing ARM toolchain.

This tree expects the Linaro 4.9 ARM EABI toolchain:
  $LINARO_URL

Quick setup:
  mkdir -p toolchain
  cd toolchain
  wget "$LINARO_URL"
  tar -xf gcc-linaro-4.9.4-2017.01-x86_64_arm-eabi.tar.xz
  mv gcc-linaro-4.9.4-2017.01-x86_64_arm-eabi arm-eabi-4.9

Then re-run:
  ./build.sh
EOF
	return 1
}

JOBS="${JOBS:-$(nproc_cmd)}"

echo "=============================="
echo "  Build RK3288 Philips U-Boot"
echo "=============================="

ensure_toolchain

echo "Using CROSS_COMPILE=${CROSS_COMPILE:-auto}"
echo "Configuring $BOARD_DEFCONFIG"
make "$BOARD_DEFCONFIG"

echo "Building and packaging with -j$JOBS"
make --jobs="$JOBS" philips-package

cat <<EOF

Build complete.

Primary outputs:
  u-boot.bin
  uboot.img
  trust.img
  rk3288_loader_v1.08.254.bin

RKDevTool package:
  flash_package/MiniLoaderAll.bin
  flash_package/parameter.txt
  flash_package/uboot.img
  flash_package/trust.img
EOF
