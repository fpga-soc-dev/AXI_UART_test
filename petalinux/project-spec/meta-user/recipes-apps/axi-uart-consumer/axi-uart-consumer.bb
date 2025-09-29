LICENSE = "CLOSED"

SRC_URI = "\
    file://CMakeLists.txt \
    file://main.cpp \
    file://UartInterface/Src/UartDevice.cpp \
    file://UartInterface/Inc/UartDevice.hpp \
    file://Utilities/Src/FileHandle.cpp \
    file://Utilities/Inc/FileHandle.hpp \
    "

S = "${WORKDIR}"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
do_rm_work[noexec] = "1"

DEPENDS += "fmt"

inherit cmake

do_install() {

    install -d ${D}/home/petalinux
    install -m 0755 ${B}/axi-uart-consumer ${D}/home/petalinux/

}


FILES:${PN} += "/home/petalinux/axi-uart-consumer"
