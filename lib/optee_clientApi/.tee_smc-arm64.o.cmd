cmd_lib/optee_clientApi/tee_smc-arm64.o := /buildpd/10BDL4551T_SZ/u-boot/../prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/arm-eabi-gcc -Wp,-MD,lib/optee_clientApi/.tee_smc-arm64.o.d  -nostdinc -isystem /buildpd/10BDL4551T_SZ/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin/../lib/gcc/arm-eabi/4.8/include -Iinclude  -I/buildpd/10BDL4551T_SZ/u-boot/arch/arm/include -include /buildpd/10BDL4551T_SZ/u-boot/include/linux/kconfig.h -D__KERNEL__ -D__UBOOT__ -DCONFIG_SYS_TEXT_BASE=0x00000000  -D__ASSEMBLY__ -g       -D__ARM__ -marm -mno-thumb-interwork  -mabi=aapcs-linux  -mword-relocations  -march=armv7-a  -mno-unaligned-access  -ffunction-sections -fdata-sections -fno-common -ffixed-r9  -msoft-float  -pipe     -c -o lib/optee_clientApi/tee_smc-arm64.o lib/optee_clientApi/tee_smc-arm64.S

source_lib/optee_clientApi/tee_smc-arm64.o := lib/optee_clientApi/tee_smc-arm64.S

deps_lib/optee_clientApi/tee_smc-arm64.o := \
    $(wildcard include/config/arm64.h) \
  include/linux/linkage.h \
  /buildpd/10BDL4551T_SZ/u-boot/arch/arm/include/asm/linkage.h \

lib/optee_clientApi/tee_smc-arm64.o: $(deps_lib/optee_clientApi/tee_smc-arm64.o)

$(deps_lib/optee_clientApi/tee_smc-arm64.o):
