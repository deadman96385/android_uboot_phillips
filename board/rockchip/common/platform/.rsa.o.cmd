cmd_board/rockchip/common/platform/rsa.o := /buildpd/10BDL4551T_SZ/u-boot/../prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/arm-eabi-gcc -Wp,-MD,board/rockchip/common/platform/.rsa.o.d  -nostdinc -isystem /buildpd/10BDL4551T_SZ/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/../lib/gcc/arm-eabi/4.8/include -Iinclude  -I/buildpd/10BDL4551T_SZ/u-boot/arch/arm/include -include /buildpd/10BDL4551T_SZ/u-boot/include/linux/kconfig.h -D__KERNEL__ -D__UBOOT__ -DCONFIG_SYS_TEXT_BASE=0x00000000 -Wall -Wstrict-prototypes -Wno-format-security -fno-builtin -ffreestanding -Os -fno-stack-protector -g -fstack-usage -Wno-format-nonliteral -D__ARM__ -marm -mno-thumb-interwork -mabi=aapcs-linux -mword-relocations -march=armv7-a -mno-unaligned-access -ffunction-sections -fdata-sections -fno-common -ffixed-r9 -msoft-float -pipe    -D"KBUILD_STR(s)=\#s" -D"KBUILD_BASENAME=KBUILD_STR(rsa)"  -D"KBUILD_MODNAME=KBUILD_STR(rsa)" -c -o board/rockchip/common/platform/rsa.o board/rockchip/common/platform/rsa.c

source_board/rockchip/common/platform/rsa.o := board/rockchip/common/platform/rsa.c

deps_board/rockchip/common/platform/rsa.o := \
  board/rockchip/common/platform/rsa.h \
  include/malloc.h \
  include/linux/stddef.h \
  include/linux/types.h \
    $(wildcard include/config/uid16.h) \
    $(wildcard include/config/use/stdint.h) \
  include/linux/posix_types.h \
  /buildpd/10BDL4551T_SZ/u-boot/arch/arm/include/asm/posix_types.h \
  /buildpd/10BDL4551T_SZ/u-boot/arch/arm/include/asm/types.h \
    $(wildcard include/config/arm64.h) \
  /buildpd/10BDL4551T_SZ/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/lib/gcc/arm-eabi/4.8/include/stdbool.h \

board/rockchip/common/platform/rsa.o: $(deps_board/rockchip/common/platform/rsa.o)

$(deps_board/rockchip/common/platform/rsa.o):
