# PY32F071-ADI-USBi

**一个廉价、开源、几乎零外围的 Analog Devices USBi 仿真器 / 调试器**
**A cheap, open-source, near-zero-BOM Analog Devices USBi debugger**

*[中文](#中文) | [English](#english)*

---

## 中文

### 项目理念

Analog Devices 官方的 USBi 仿真器（`EVAL-ADUSB2EBZ`）是 SigmaStudio 与 ADAU 系列 DSP 之间的桥梁。它的核心是一颗 **Cypress CY7C68013A / CY7C68053（FX2LP / FX2LP18）**——一颗 2000 年代的 8051 + USB 2.0 全速控制器，必须配 24 MHz 外部晶振，还要外挂一颗 I2C EEPROM 存放固件。整个方案器件多、工具链老、芯片也不便宜。

**本项目想做的事很简单：用一颗几块钱的现代 ARM Cortex-M0+ 单片机，把这颗老芯片完整替代掉。**

于是有了这个基于 **PY32F071K1BU6** 的"虚拟 USBi"：

* 对 SigmaStudio 表现的形状与官方 USBi **完全一致**（同样的 VID/PID、同样的零端点单接口、同样的 EP0 vendor 请求），插上去就能被识别，不需要改任何上位机配置；
* **不需要外部晶振**：内部 HSI 24 MHz 经 PLL ×2 得到 48 MHz 系统时钟；固件直接跑在片内 128 KB Flash 里，**不需要外挂 EEPROM**；
* **几乎不需要任何外围器件**：除了 USB 接口本身和 4 根连到 DSP 的控制线，只需要**一颗 WS2812 灯珠**；
* 用这颗 WS2812 当调试指示灯，**工作状态一眼就能看清**：连没连上、走的是哪条总线、是在写还是在读、是不是在烧 EEPROM，全用颜色和闪法区分（见下表）。

### 功能状态

| 功能 | 状态 |
|---|---|
| USB 全速设备枚举（VID `0x0456` / PID `0x7031`，单接口 class `0xFF`，零端点） | ✅ 已实现并验证 |
| EP0 vendor 请求：`0xB2` 写寄存器 / `0xB3` 读请求 / `0xB4` 读响应 / `0xB5`·`0xB6` ACK | ✅ 已实现并验证 |
| SPI 从机口调试：寄存器读写、程序/参数下载、safeload、写后回读 | ✅ 已实现并验证 |
| > 4096 字节的超长块写（EP0 流式接收，256 字节一块边收边发） | ✅ 已实现（用于 EEPROM 烧录流程中的 12568 字节整幅镜像） |
| 目标复位线控制（PB0，常态输出高） | ✅ 已实现 |
| WS2812 状态指示 | ✅ 已实现 |
| **I2C 从机口调试** | ❌ **未实现，等待社区共创** |

### 为什么 I2C 还没做

ADAU145x 的从机控制口**上电默认是 I2C**，但一旦被连续拉低 3 次 `SS/ADDR0`（SigmaStudio 打开工程时会自己发 3 次 "SPI Toggle"）就会**永久锁进 SPI 模式**；要回到 I2C 只能靠 RESET 引脚做硬件复位或者断电重启，写 `0xF890` 软复位也不好使。

也就是说 SPI 和 I2C 之间是一道**单向门**，而 I2C 还需要：

1. 硬件上把 DSP 的 `CCLK/SCL`、`MISO/SDA` 接到单片机的 I2C 引脚，并且**两条线各挂一个 2 kΩ 上拉**；
2. 地址脚 `SS/ADDR0`、`MOSI/ADDR1`（决定从机地址 `0x38`~`0x3B`）在运行期间保持稳定电平。

本项目的 I2C 后端其实**代码已经写完了**（地址探测、分块写入、程序区字间延时、地址语义映射），但目前**没有硬件条件验证**，所以整段被 `#if 0` 归档在 `firmware/Core/SRC/usbi.c` 的"V1.0 存档：I2C 后端"一节里（`I2C1` 外设本身的初始化保留未删）。**欢迎有硬件的社区朋友把它接上、验证、PR 回来。**

### 硬件

| 项目 | 说明 |
|---|---|
| MCU | PY32F071K1BU6（Cortex-M0+，48 MHz，128 KB Flash / 16 KB RAM，QFN32） |
| 时钟 | 内部 HSI 24 MHz + PLL ×2 = 48 MHz，**无外部晶振** |
| USB | 全速 12 Mbps 设备，仅用 EP0；`D+` 需 **1.5 kΩ 上拉到 3.3 V**（全速设备标识） |
| 外围器件 | 1 颗 WS2812(B) 灯珠 + 若干去耦电容，**无 EEPROM、无晶振** |
| 资源占用 | `text 19032 / data 4216 / bss 3336` → RAM 6672 / 16384 B，Flash ≈ 23 KB / 128 KB |

### 引脚接线

| PY32F071 | 功能 | 接到 ADAU145x 从机控制口 |
|---|---|---|
| `PA4` | SPI1_CS（软件控制） | `SS/ADDR0`（pin33） |
| `PA5` | SPI1_SCK | `CCLK/SCL`（pin31） |
| `PA6` | SPI1_MISO | `MISO/SDA`（pin30） |
| `PA7` | SPI1_MOSI | `MOSI/ADDR1`（pin32） |
| `PB0` | 目标 RESET 输出（空闲高 = 释放） | `RESET`（pin24） |
| `PB9` | WS2812 数据（TIM17_CH1 + DMA1_Ch1） | WS2812 DIN |
| `PA11` / `PA12` | USB D- / D+（USB 外设自动接管） | USB 口 |

SPI 参数：主机模式、Mode 0（CPOL=0 / CPHA=1Edge）、MSB first、8 bit、分频 16 → **3 MHz**。`SS` 由软件在每次事务前后拉低/拉高。

ADAU 地址语义：`>= 0xF000` 为寄存器，按字节递增；`< 0xF000` 为存储器，按 4 字节字递增（`usbi.c` 的 `bus_write_at()` 已处理）。

### 预编译固件

不想折腾编译环境的，直接烧这两个文件：

| 文件 | 说明 |
|---|---|
| `firmware/prebuilt/Alternative_ADI_USBI.hex` | Intel HEX，用 PY32 DFU 工具 / 编程器直接下载 |
| `firmware/prebuilt/Alternative_ADI_USBI.bin` | 原始二进制镜像（烧录起始地址 `0x08000000`） |

（对应当前 V1.0 源码，`text 19032 / data 4216 / bss 3328`。）

### 安装驱动（Windows）

Windows 默认不认识这个设备，需要手动指定 Cypress 的驱动：

1. 把板子插上 USB。打开**设备管理器**，会看到一个带黄色感叹号的 **未知设备 / Unknown Device**；
2. 右键该设备 → **更新驱动程序** → **浏览我的电脑上的驱动程序**；
3. 点"浏览"，选择本仓库 `driver\` 下**与你系统对应**的文件夹（Win10 / Win11 64 位选 `driver\Win10\x64`，32 位选 `driver\Win10\x86`；Win7 选 `driver\Win7\x64`，Win8 / 8.1 / Vista / XP 以此类推），勾选**包括子文件夹**，确定；
4. 安装完成后，设备名会变成 **Cypress FX2LP No EEPROM Device** —— 这时 SigmaStudio 就能认出 USBi 了。

> **说明**：这是**原版社区 CY7C68013A 版 USBi 的驱动安装过程**，本项目直接借用了 Cypress 的驱动层（CyUSB3）。之所以能这么做，是因为固件对外的 VID/PID 与描述符形状（单接口 `class 0xFF`、零端点）和官方 USBi 完全一致，SigmaStudio 和 CyUSB 认的正是这一层。
> 文件夹里的 `ADI_USBi.spt` 是官方 USBi 的 8051 固件镜像，**本项目不需要它**，留在这里仅作参考。

### 烧录固件（PY32 DFU）

1. 把 **`PF8`（BOOT0）引脚拉低**，然后给板子上电 —— 单片机进入 ROM bootloader，不再运行用户程序；
2. 用 USB 线把板子连到电脑；
3. 打开 `PY32_DfuTool_V1.1.2\PY32DfuTool_x64.exe`（32 位系统用 `_x86`），工具里会自动识别到设备；
4. 选择 `firmware/prebuilt/Alternative_ADI_USBI.hex`（或自己编译出来的 hex），点下载；
5. 烧完断开 BOOT0 拉低（恢复悬空），**重新上电**，固件就开始运行了。

### LED 状态说明

| 状态 | 颜色 / 表现 |
|---|---|
| 未连接主机 | 白色慢呼吸 `(64,64,64)` |
| 已连接，空闲（SPI 模式） | 绿色呼吸 `(0,64,24)` |
| 已连接，空闲（I2C 模式，归档） | 紫色呼吸 `(64,0,107)` |
| 正在**写入** DSP | 蓝色高频脉冲 `(0,48,127)` |
| 正在**读出** DSP | 青色低频脉冲 `(0,127,127)` |
| 正在烧录 EEPROM | 红色高频闪 `(127,8,0)` |

### 编译与使用

1. 用 **EIDE** / VS Code 打开 `firmware/EIDE/Alternative_ADI_USBI.code-workspace`（工程清单在 `firmware/EIDE/.eide/`，新增 `.c` 文件必须登记进去）；
2. 编译器：`arm-none-eabi-gcc`（`-mcpu=cortex-m0plus -mthumb`）；
3. 编译产物烧进芯片（SWD 或 `PY32_DfuTool`）；
4. 把 4 根控制线 + RESET 接到目标 DSP 板，插上 USB；
5. 打开 SigmaStudio 4.7，硬件配置里选 USBi，即可正常下载程序、读写参数。

### 目录结构

```
main/
├─ driver/                 Cypress CyUSB3 驱动（按系统分文件夹，见上文驱动安装）
├─ firmware/               ← 本项目的 EIDE 固件工程
│  ├─ prebuilt/            预编译固件（Alternative_ADI_USBI.hex / .bin，V1.0）
│  ├─ Core/                main.c、usbi.c（协议+总线后端）、usbi_led.c、ws2812.c 等
│  ├─ Drivers/             PY32F071 HAL / CMSIS
│  ├─ Middlewares/         CherryUSB USB 协议栈 + FreeRTOS
│  └─ EIDE/                工程清单、链接脚本、启动文件
├─ hardware/               原理图
└─ PY32_DfuTool_V1.1.2/    官方串口下载工具
```

### 参考与致谢

* `rp2040-Analog-Devices-USBi`（Rust / embassy，真正的 USB USBi 仿真器实现）
* `adau1452-esp32-controller`（I2C 分块与延时、ADAU145x 地址语义参考）
* ADI《ADAU1452/1451/1450》数据手册 Rev.D（控制口时序、I2C/SPI 模式切换、下载流程）
* CherryUSB（USB device 协议栈）

> 本项目为个人学习与开源实践，与 Analog Devices 无关；ADAU、SigmaStudio、USBi 是 Analog Devices 的商标。

---

## English

### Why this project exists

Analog Devices' official **USBi** in-circuit debugger (`EVAL-ADUSB2EBZ`) is the bridge between SigmaStudio and the ADAU DSP family. Its core is a **Cypress CY7C68013A / CY7C68053 (FX2LP / FX2LP18)** — a 2000s-era 8051 + USB full-speed controller that needs a 24 MHz crystal and an external I²C EEPROM to hold its firmware. Many parts, an old toolchain, and a chip that is not exactly cheap.

**The idea is simple: replace that legacy chip with one modern, inexpensive ARM Cortex-M0+ MCU.**

This repository is a **PY32F071K1BU6-based "virtual USBi"**:

* It presents **exactly the same shape** to SigmaStudio as the official USBi (same VID/PID, same single interface with zero endpoints, same EP0 vendor requests) — plug it in and it is recognized, with no host-side configuration changes.
* **No external crystal**: the internal HSI (24 MHz) is doubled by the PLL to a 48 MHz system clock, and the firmware lives in the on-chip 128 KB flash — **no external EEPROM either**.
* **Almost no external components**: besides the USB pins and the four control lines to the DSP, all it needs is **a single WS2812 LED**.
* That WS2812 doubles as a debug indicator, so **the current state is obvious at a glance**: whether the host is connected, which bus is in use, whether it is writing or reading, and whether an EEPROM burn is in progress — each with its own colour and blink pattern (see the table above).

### Status

| Feature | Status |
|---|---|
| USB full-speed enumeration (VID `0x0456` / PID `0x7031`, single class-`0xFF` interface, zero endpoints) | ✅ Implemented & verified |
| EP0 vendor requests: `0xB2` write register / `0xB3` read request / `0xB4` read response / `0xB5`, `0xB6` ACK | ✅ Implemented & verified |
| SPI slave-port debugging: register read/write, program & parameter download, safeload, read-back | ✅ Implemented & verified |
| Block writes larger than 4096 bytes (streamed EP0 OUT, 256-byte chunks forwarded on the fly) | ✅ Implemented (for the 12568-byte image in the EEPROM burn flow) |
| Target reset line control (`PB0`, idles high) | ✅ Implemented |
| WS2812 status LED | ✅ Implemented |
| **I²C slave-port debugging** | ❌ **Not implemented — community contributions welcome** |

### Why I²C is not done yet

The ADAU145x slave control port **powers up in I²C mode**, but once `SS/ADDR0` is pulled low three times in a row (SigmaStudio does this itself as three "SPI Toggle" writes when you open a project) the port is **latched into SPI mode permanently**. The only way back to I²C is a hardware reset via the `RESET` pin or a power cycle — the `0xF890` soft reset will not do it.

So SPI and I²C are separated by a **one-way door**, and I²C additionally requires:

1. The DSP's `CCLK/SCL` and `MISO/SDA` wired to the MCU's I²C pins, **each with a 2 kΩ pull-up**;
2. The address straps `SS/ADDR0` and `MOSI/ADDR1` (which select slave address `0x38`–`0x3B`) held stable while the device is running.

The I²C backend is in fact **already written** (address probing, chunked writes, inter-word delays for the program region, ADAU address semantics), but there was **no hardware available to verify it**, so the whole thing is archived behind `#if 0` in the "V1.0 archive: I²C backend" section of `firmware/Core/SRC/usbi.c` (the `I2C1` peripheral initialisation is kept). **If you have the hardware, please wire it up, verify it and send a PR.**

### Hardware

| Item | Detail |
|---|---|
| MCU | PY32F071K1BU6 (Cortex-M0+, 48 MHz, 128 KB flash / 16 KB RAM, QFN32) |
| Clock | Internal HSI 24 MHz × PLL2 = 48 MHz, **no external crystal** |
| USB | Full-speed 12 Mbps device, EP0 only; `D+` needs a **1.5 kΩ pull-up to 3.3 V** (full-speed device signature) |
| External parts | One WS2812(B) LED plus decoupling, **no EEPROM, no crystal** |
| Footprint | `text 19032 / data 4216 / bss 3336` → RAM 6672 / 16384 B, flash ≈ 23 KB / 128 KB |

### Wiring

| PY32F071 | Function | ADAU145x slave control port |
|---|---|---|
| `PA4` | SPI1_CS (software driven) | `SS/ADDR0` (pin33) |
| `PA5` | SPI1_SCK | `CCLK/SCL` (pin31) |
| `PA6` | SPI1_MISO | `MISO/SDA` (pin30) |
| `PA7` | SPI1_MOSI | `MOSI/ADDR1` (pin32) |
| `PB0` | Target `RESET` output (idle high = released) | `RESET` (pin24) |
| `PB9` | WS2812 data (TIM17_CH1 + DMA1_Ch1) | WS2812 DIN |
| `PA11` / `PA12` | USB D- / D+ (taken over by the USB peripheral) | USB connector |

SPI: master, Mode 0 (CPOL=0 / CPHA=1Edge), MSB first, 8-bit, prescaler 16 → **3 MHz**; `SS` is driven low/high in software around each transaction.

ADAU addressing: addresses `>= 0xF000` are registers (byte-incrementing) and `< 0xF000` are memories (4-byte word incrementing); see `bus_write_at()` in `usbi.c`.

### Prebuilt firmware

No toolchain required — flash one of these:

| File | Notes |
|---|---|
| `firmware/prebuilt/Alternative_ADI_USBI.hex` | Intel HEX, ready for the PY32 DFU tool or any programmer |
| `firmware/prebuilt/Alternative_ADI_USBI.bin` | Raw binary image (load address `0x08000000`) |

(Built from the current V1.0 sources: `text 19032 / data 4216 / bss 3328`.)

### Installing the driver (Windows)

Windows does not know this device out of the box, so you have to point it at the Cypress driver manually:

1. Plug the board into USB. In **Device Manager** an **Unknown Device** appears with a yellow warning triangle;
2. Right-click it → **Update driver** → **Browse my computer for drivers**;
3. Click *Browse* and select the folder under `driver\` that **matches your OS** (Windows 10 / 11 64-bit → `driver\Win10\x64`, 32-bit → `driver\Win10\x86`; Windows 7 → `driver\Win7\x64`, and likewise for Win8 / 8.1 / Vista / XP). Keep **Include subfolders** checked and confirm;
4. Once it finishes, the device is renamed to **Cypress FX2LP No EEPROM Device** — SigmaStudio will now find the USBi.

> **Note**: this is the very same driver installation procedure as the **original community CY7C68013A-based USBi**; this project simply reuses the Cypress driver layer (CyUSB3). That works because the firmware presents exactly the same VID/PID and descriptor shape as the official USBi (single `class 0xFF` interface, zero endpoints) — which is precisely the layer SigmaStudio and CyUSB look at.
> The `ADI_USBi.spt` file in that folder is the official USBi's 8051 firmware image; **this project does not need it** — it is kept for reference only.

### Flashing the firmware (PY32 DFU)

1. Pull the **`PF8` (BOOT0) pin low**, then power the board up — the MCU enters its ROM bootloader instead of running the application;
2. Connect the board to your PC with a USB cable;
3. Launch `PY32_DfuTool_V1.1.2\PY32DfuTool_x64.exe` (use `_x86` on 32-bit systems); the tool detects the device automatically;
4. Select `firmware/prebuilt/Alternative_ADI_USBI.hex` (or your own build), then click download;
5. Release BOOT0 (leave it floating) and **power-cycle the board** — the new firmware starts running.

### Build & use

1. Open `firmware/EIDE/Alternative_ADI_USBI.code-workspace` with **EIDE** / VS Code (the project manifest is under `firmware/EIDE/.eide/`; new `.c` files must be registered there);
2. Toolchain: `arm-none-eabi-gcc` (`-mcpu=cortex-m0plus -mthumb`);
3. Flash the firmware (SWD or `PY32_DfuTool`);
4. Wire the four control lines plus `RESET` to your DSP board and plug in USB;
5. Open SigmaStudio 4.7, select USBi in the hardware configuration, and download programs / read and write parameters as usual.

### Credits

* `rp2040-Analog-Devices-USBi` (Rust / embassy — a real USB USBi emulator implementation)
* `adau1452-esp32-controller` (I²C chunking/delays and ADAU145x addressing reference)
* Analog Devices *ADAU1452/1451/1450* datasheet Rev.D (control-port timing, I²C/SPI mode latching, download procedure)
* CherryUSB (USB device stack)

> A personal learning and open-source project, not affiliated with Analog Devices. ADAU, SigmaStudio and USBi are trademarks of Analog Devices.
