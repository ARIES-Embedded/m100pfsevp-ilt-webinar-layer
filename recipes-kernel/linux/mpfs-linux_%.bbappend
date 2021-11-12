SRC_URI += "file://m100pfsevp-ilt.dts;subdir=git/arch/${ARCH}/boot/dts/aries"

FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"

PACKAGE_ARCH = "${MACHINE_ARCH}"

KERNEL_DEVICETREE += "aries/m100pfsevp-ilt.dtb"

#do_configure_prepend_m100pfsevp() {
#        cp -f ${WORKDIR}/m100pfsevp-ilt.dts ${S}/arch/riscv/boot/dts/aries
#}

