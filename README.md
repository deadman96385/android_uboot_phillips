# Philips 10BDL4551T U-Boot

This repository contains a Rockchip vendor U-Boot tree configured for the Philips 10BDL4551T display. The hardware platform is based on the Rockchip RK3288 SoC.

## Purpose

This tree builds:

- U-Boot for the Philips 10BDL4551T
- Rockchip packaging artifacts used by RKDevTool / Rockchip flashing workflows

## Quick Start

Run:

```sh
./build.sh
```

`build.sh` is the intended entrypoint. It will:

1. Check for an `arm-eabi` toolchain
2. Use `toolchain/arm-eabi-4.9` automatically if present
3. Prompt to download the expected Linaro toolchain if missing
4. Run `make rk3288_philips_defconfig`
5. Build U-Boot
6. Create the Philips flash package

## Toolchain

The expected toolchain is the Linaro 4.9 ARM EABI release:

`https://releases.linaro.org/components/toolchain/binaries/4.9-2017.01/arm-eabi/gcc-linaro-4.9.4-2017.01-x86_64_arm-eabi.tar.xz`

If no compatible `arm-eabi-gcc` is available, `build.sh` will ask whether it should download and install this toolchain into:

`toolchain/arm-eabi-4.9`

## Outputs

The main build outputs are:

- `u-boot.bin`
- `uboot.img`
- `trust.img`
- `rk3288_loader_v1.08.254.bin`

## Flash Package

`build.sh` also creates:

- `flash_package/MiniLoaderAll.bin`
- `flash_package/parameter.txt`
- `flash_package/uboot.img`
- `flash_package/trust.img`

These are the files intended for Rockchip flashing tools.

The package layout is:

- `MiniLoaderAll.bin` -> loader
- `parameter.txt` -> partition parameter file
- `uboot.img` -> U-Boot image
- `trust.img` -> trust / TEE image

## Board-Specific Files

Philips-specific files live in:

- `configs/rk3288_philips_defconfig`
- `arch/arm/dts/rk3288-philips.dts`
- `board/rockchip/rk32xx/philips/parameter.txt`

## Notes

- `flash_package/` is generated output and is ignored by git.
- `toolchain/` is treated as a local-only directory and is ignored by git.
- This is a vendor tree, so warnings during build are expected unless separately cleaned up.
