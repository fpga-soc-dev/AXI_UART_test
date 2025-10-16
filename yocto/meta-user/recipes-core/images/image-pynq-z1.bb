SUMMARY = "Image for pynq z1 board"

IMAGE_LINGUAS = " "

LICENSE = "MIT"

inherit core-image

IMAGE_INSTALL = "packagegroup-core-boot"
IMAGE_INSTALL:append = " ${CORE_IMAGE_EXTRA_INSTALL}"
IMAGE_INSTALL:append = " axi-uart-loopback-test axi-uart-producer axi-uart-consumer"
