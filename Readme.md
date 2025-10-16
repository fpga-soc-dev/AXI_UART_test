
# AXI UART IP Example 

## Setup Environment
- Vivado 2024.2
- Yocto
- PYNQ Z1 Board
- Ubuntu 22.04


## Build Vivado Project
```bash
source <vivado_shell_script>
cd Vivado
make
```

## Build Yocto Project
Check requirements for Yocto Build

[Preparing host build](https://docs.yoctoproject.org/singleindex.html#preparing-the-build-host)

[System Requirements](https://docs.yoctoproject.org/scarthgap/ref-manual/system-requirements.html#system-requirements)


Install KAS tool
```bash
pip install kas
```

Build project
```bash
cd yocto
make
```

Write generated .wic image(under `yocto/build/tmp-glibc/deploy/images/pynq-z1/image-pynq-z1-pynq-z1.rootfs.wic` folder) using tools such as Balana Etcher.
