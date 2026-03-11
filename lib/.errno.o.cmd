cmd_lib/errno.o := /buildpd/10BDL4551T_SZ/u-boot/../prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/arm-eabi-gcc -Wp,-MD,lib/.errno.o.d  -nostdinc -isystem /buildpd/10BDL4551T_SZ/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/../lib/gcc/arm-eabi/4.8/include -Iinclude  -I/buildpd/10BDL4551T_SZ/u-boot/arch/arm/include -include /buildpd/10BDL4551T_SZ/u-boot/include/linux/kconfig.h -D__KERNEL__ -D__UBOOT__ -DCONFIG_SYS_TEXT_BASE=0x00000000 -Wall -Wstrict-prototypes -Wno-format-security -fno-builtin -ffreestanding -Os -fno-stack-protector -g -fstack-usage -Wno-format-nonliteral -D__ARM__ -marm -mno-thumb-interwork -mabi=aapcs-linux -mword-relocations -march=armv7-a -mno-unaligned-access -ffunction-sections -fdata-sections -fno-common -ffixed-r9 -msoft-float -pipe    -D"KBUILD_STR(s)=\#s" -D"KBUILD_BASENAME=KBUILD_STR(errno)"  -D"KBUILD_MODNAME=KBUILD_STR(errno)" -c -o lib/errno.o lib/errno.c

source_lib/errno.o := lib/errno.c

deps_lib/errno.o := \

lib/errno.o: $(deps_lib/errno.o)

$(deps_lib/errno.o):
