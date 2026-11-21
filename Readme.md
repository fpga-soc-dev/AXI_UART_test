
# AXI UART Lite IP Core Example — PYNQ-Z1

A minimal, end-to-end example of using the **Xilinx AXI UART Lite IP core** on a
PYNQ-Z1 (Zynq-7020) board. Two UART Lite cores are instantiated and exercised
from Linux with a userspace C++ test suite (loopback, producer, consumer).

The project covers the full stack:

1. **Hardware (Vivado)** — block design with two `axi_uartlite` cores connected
   to the PS.
2. **Linux (Yocto)** — a Scarthgap image whose device tree is generated from the
   exported `.xsa`, exposing the cores as `/dev/ttyUL1` and `/dev/ttyUL2`.
3. **Application (C++20)** — a `termios`-based UART wrapper plus three test apps
   that validate the link with random data.

---

## Overview

| Item        | Value                                                |
|-------------|------------------------------------------------------|
| Board       | PYNQ-Z1 (Zynq-7020, `xc7z020clg400-1`)               |
| IP cores    | 2 × `xilinx.com:ip:axi_uartlite` @ **0x42C0_0000** / **0x42C1_0000** |
| Baud rate   | 230400 (8N1)                                         |
| UART0 pins  | `TX` → **Y18**, `RX` → **Y19**                       |
| UART1 pins  | `TX` → **U18**, `RX` → **U19**                       |
| Linux devices| `/dev/ttyUL1` (UART0), `/dev/ttyUL2` (UART1)        |
| Toolchain   | Vivado **2024.2**, Yocto **Scarthgap** (kas)         |

## Setup environment

- Vivado 2024.2
- Yocto Scarthgap + [kas](https://kas.readthedocs.io/)
- PYNQ-Z1 board
- Ubuntu 22.04 (or a similar Linux host)

## Hardware design

### Block design (`Vivado/scripts/pynq_z1_bd.tcl`)

The block design contains:

- `processing_system7` — the Zynq ARM Cortex-A9 PS.
- `axi_uartlite_0` — first UART core:
  - `CONFIG.C_BAUDRATE = 230400`.
  - Base address `0x42C0_0000`.
- `axi_uartlite_1` — second UART core:
  - `CONFIG.C_BAUDRATE = 230400`.
  - Base address `0x42C1_0000`.
- `proc_sys_reset`, `xlconcat`, `smartconnect` — reset / interrupt / AXI glue.

### Top level & pins

`Vivado/src/pynq_z1_top.vhd` maps the block-design ports to board pins.
`Vivado/src/pynq_z1.xdc`:

```tcl
# AXI UART0
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {AXI_UART0_TX}]
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS33} [get_ports {AXI_UART0_RX}]
# AXI UART1
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports {AXI_UART1_TX}]
set_property -dict {PACKAGE_PIN U19 IOSTANDARD LVCMOS33} [get_ports {AXI_UART1_RX}]
```

### Loopback wiring

The loopback test sends data **UART0 → UART1** and **UART1 → UART0**, so the two
cores must be cross-wired externally:

| From  | To   |
|-------|------|
| UART0 TX (Y18) | UART1 RX (U19) |
| UART1 TX (U18) | UART0 RX (Y19) |

## Linux / device tree

The two `axi_uartlite` nodes are **generated** from the exported `.xsa` by
`meta-xilinx-tools` and bound to the Linux `uartlite` driver, appearing as
`/dev/ttyUL1` and `/dev/ttyUL2` (the paths the apps use). `system-user.dtsi`
only adds board-level pieces (console bootargs, Ethernet PHY, USB, SPI flash).

## Applications

The shared library lives in `apps/` and is symlinked into each recipe:

- `apps/UartInterface` — `UartDevice`: a `termios`-based wrapper (`Read`,
  `Write`, async data callbacks) around the tty device node.
- `apps/Utilities` — `FileHandle`: RAII wrapper around `open()` / `close()`.

Three executables are built, each with its own `main.cpp`/`CMakeLists.txt` in
the corresponding Yocto recipe:

| App | Binary | Purpose |
|-----|--------|---------|
| `axi-uart-loopback-test` | `/home/root/uart-loopback` | Random-data loopback test between UART0 and UART1 |
| `axi-uart-producer`       | `/home/root/axi-uart-producer` | Sends random data and waits for a response |
| `axi-uart-consumer`       | `/home/root/axi-uart-consumer` | Echo server (receives data and echoes it back) |

Device paths used by the loopback test (`main.cpp`):

```cpp
constexpr auto UART0_DEVICE = "/dev/ttyUL1";
constexpr auto UART1_DEVICE = "/dev/ttyUL2";
```

The producer/consumer take the device path as an argument:

```bash
/home/root/axi-uart-producer /dev/ttyUL1
/home/root/axi-uart-consumer /dev/ttyUL2
```

## Build

### 1. Hardware — Vivado

```bash
source <Vivado_install>/settings64.sh
cd Vivado
make
```

`make` runs the Tcl scripts to create the project, build the bitstream, and
export `pynq_z1_top.xsa` (plus `pynq_z1_top.bit`) at the `Vivado/` root.

### 2. Linux image — Yocto (kas)

Prepare the host for Yocto:

- [Preparing the build host](https://docs.yoctoproject.org/singleindex.html#preparing-the-build-host)
- [System requirements (Scarthgap)](https://docs.yoctoproject.org/scarthgap/ref-manual/system-requirements.html#system-requirements)

Install `kas`:

```bash
pip install kas
```

Build:

```bash
cd yocto
make
```

The machine config (`yocto/meta-user/conf/machine/zynq-generic-7z020.conf`) reads
the hardware design directly via `HDF_PATH = ".../Vivado/pynq_z1_top.xsa"`, so
step 1 must run first. `kas build build.yaml` then produces the SD-card image —
the `.wic` file under `yocto/build/tmp-glibc/deploy/images/zynq-generic-7z020/`
(e.g. `image-pynq-z1-zynq-generic-7z020.rootfs.wic`).

### 3. Flash & run

1. Flash the `.wic` image to a microSD card (e.g. with Balena Etcher).
2. Insert the card, connect the USB UART (115200 8N1), and power the board.
3. Log in as `root` and run the loopback test:

```bash
/home/root/uart-loopback
```

Expected output:

```
Starting UART loopback test...
UART0: /dev/ttyUL1 - Opened
UART1: /dev/ttyUL2 - Opened

=== Synchronous Test Iteration 1 ===
UART0 wrote 21642 bytes
UART1 read 21642 bytes
UART1 wrote 21642 bytes
UART0 read 21642 bytes
UART Test: PASS
...
```

## Repository layout

```
.
├── Vivado/                  # Hardware: block design, VHDL top, constraints
│   ├── scripts/             #   create_project.tcl, pynq_z1_bd.tcl, build_bitstream.tcl
│   └── src/                 #   pynq_z1_top.vhd, pynq_z1.xdc
├── yocto/                   # Linux image: build.yaml + meta-user layer
│   └── meta-user/
│       ├── recipes-bsp/     #   device-tree, u-boot
│       ├── recipes-kernel/  #   kernel config
│       └── recipes-apps/    #   axi-uart-{loopback-test,producer,consumer}
└── apps/                    # Shared UART + utility C++ source
    ├── UartInterface/       #   UartDevice (termios)
    └── Utilities/           #   FileHandle (RAII)
```

## References

- [AXI UART Lite v2.0 — Product Guide (PG142)](https://docs.amd.com/r/en-US/pg142-axi-uartlite)
- [PYNQ-Z1 Reference Manual](https://digilent.com/reference/programmable-logic/pynq-z1/reference-manual)
