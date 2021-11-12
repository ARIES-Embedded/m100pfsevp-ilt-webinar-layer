SUMMARY = "bitbake recipe for the M100PFSWVP ILT userspace driver"
DESCRIPTION = "Application to demonstrate Polarfire IP-core access via UIO-Driver "
LICENSE = "MIT"
# This is just a simple recipe for a quick demonstration in the ArrowToGo webinar
LIC_FILES_CHKSUM = "file://LICENSE;md5=06ec214e9fafe6d4515883d77674a453"


FILESEXTRAPATHS_prepend := "${THISDIR}/files:"

SRC_URI = "file://Makefile \
            file://uio-aries-ilt.c \
            file://LICENSE "
 
# Set LDFLAGS options provided by the build system
TARGET_CC_ARCH += "${LDFLAGS}"
 
# Change source directory to workdirectory 
S = "${WORKDIR}"

# Install binary to final directory /usr/bin
do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/uio-aries-ilt ${D}${bindir}
}

