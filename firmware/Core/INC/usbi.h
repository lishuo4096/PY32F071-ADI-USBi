/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbi.h
  * @brief   Analog Devices USBi (EVAL-ADUSB2EBZ) 兼容仿真器 —— 上层接口定义
  *
  *          本模块让 PY32F071 变成一颗"虚拟 USBi"：USB 侧只呈现
  *          VID 0x0456 / PID 0x7031 的单一 vendor 接口（class 0xFF、零端点），
  *          主机侧 SigmaStudio 经 CyUSB.sys 打开后只用 EP0 收发 0xB2~0xB6
  *          五个 vendor 请求；本模块把寄存器读写翻译成对目标 SigmaDSP 的
  *          SPI 或 I2C 访问。
  *
  *          总线：V1.0 只用 SPI（I2C 后端已归档，见 usbi.c 的存档段）。
  *          描述符 / vendor 请求 / SPI 时序三部分与总线无关，仍是完整的 USBi。
  *
  *          注意：ADAU145x 的从机口**上电默认 I2C**，只有把 SS/ADDR0 拉低/拉高
  *          3 次才会进 SPI 模式；而回到 I2C 只能靠硬件 RESET 或断电（软复位
  *          0xF890 也不行）。
  *
  *          "切进 SPI 模式"这件事**不需要固件主动做**：SigmaStudio 下载时会自己
  *          连发 3 次 "SPI Toggle"（向 0x4FFF 写 4 字节 0x00，中间夹
  *          "SPI Toggle Delay"），每次 SPI 事务天然在 SS/ADDR0 上产生一个低-高
  *          脉冲，三次脉冲正好把目标从 I2C 模式切到 SPI 模式。因此本模块只负责
  *          把主机的写请求照原样搬到总线上，不再自己发起模式切换脉冲。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USBI_H__
#define __USBI_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* ===================== 目标总线选择（编译期，真正生效） ===================== */
/* 本宏决定"USB 控制端点背后那条总线"走哪组硬件：
 *   USBI_BUS_SPI  —— SPI1（PA4 软件片选 + PA5 SCK + PA6 MISO + PA7 MOSI），默认
 *   USBI_BUS_I2C  —— 硬件 I2C1（400 kHz，默认 PA9=SCL / PA10=SDA，见下面的引脚宏）
 *   USBI_BUS_AUTO —— 上电先探测 I2C 从机（0x38~0x3B），探测到就走 I2C，否则退回 SPI
 *
 * USB 协议层完全不变：两档都是同一套 0xB2~0xB6 vendor 请求，差别只在"收到写
 * 请求后往哪条总线上发"。SigmaStudio 侧的 wValue/wIndex 也照旧被忽略
 * （参考工程 adau1452-esp32-controller 同样硬编码地址与 format）。
 *
 * ⚠ 从 SPI 切回 I2C 必须先硬件复位目标：ADAU145x 上电默认 I2C 从口，
 *   SS/ADDR0 被拉低 3 次后永久进入 SPI 模式，软复位 0xF890 也回不来。
 *
 * ⚠ 硬件前提：I2C 的 SCL/SDA 各需要 2 kΩ 外部上拉。若板上只有 MCU 内部上拉
 *   （约 30~50 kΩ），请把 USBI_I2C_CLOCK_HZ 降到 20000 及以下，否则上升沿
 *   跟不上、读回全是 0xFF。
 */
#define USBI_BUS_SPI  0
#define USBI_BUS_I2C  1
#define USBI_BUS_AUTO 2

#ifndef USBI_BUS_SELECT
#define USBI_BUS_SELECT USBI_BUS_SPI
#endif

#if (USBI_BUS_SELECT != USBI_BUS_SPI) && \
    (USBI_BUS_SELECT != USBI_BUS_I2C) && \
    (USBI_BUS_SELECT != USBI_BUS_AUTO)
#error "USBI_BUS_SELECT 只能是 USBI_BUS_SPI / USBI_BUS_I2C / USBI_BUS_AUTO"
#endif

/* SPI 片选引脚（mcustudio 里配的是 SPI1 硬件 NSS = PA4，
 * 本模块会在 usbi_init() 里把它收回当成普通 GPIO 用） */
#define USBI_SPI_CS_PORT GPIOA
#define USBI_SPI_CS_PIN  GPIO_PIN_4

/* ==================== I2C 引脚（硬件 I2C1） ==================== */
/* 用硬件 I2C1 + HAL：正版 USBi 的 DSP 侧 I2C 固定 400 kHz，外设能跑满标称速率，
 * 抖动也比软件模拟小。
 *
 * ⚠ 但 HAL 的每个等待都是 `while (!flag) { if (HAL_GetTick()-t0 > Timeout) 报错; }`，
 *   而 vendor 请求全在 USB 中断里执行 —— 如果 SysTick 的优先级低于 USB 中断，
 *   中断期间 uwTick 不前进，超时判断永远为假，总线一被拉死就会永久卡住。
 *   所以本工程把 SysTick 抬到最高优先级、USB 中断降到 1（见 py32f071_hal_conf.h
 *   的 TICK_INT_PRIORITY 与 Core/SRC/usb.c 的 HAL_NVIC_SetPriority(USB_IRQn,1,0)）。
 *
 * 引脚可换：USBI_I2C_ON_SPI_PINS = 1 时改用 SPI 那组线（PA5=SCL、PA7=SDA）。
 * 真 USBi 的排针上 ADAU 的 I2C 与被控 SPI 是同一组物理线
 * （SCL/CCLK、SDA/MOSI、SS/ADDR0），按真 USBi 10 针口引出的板子必须置 1。 */
#ifndef USBI_I2C_ON_SPI_PINS
#define USBI_I2C_ON_SPI_PINS 0
#endif

#if USBI_I2C_ON_SPI_PINS
#define USBI_I2C_SCL_PORT GPIOA
#define USBI_I2C_SCL_PIN  GPIO_PIN_5
#define USBI_I2C_SDA_PORT GPIOA
#define USBI_I2C_SDA_PIN  GPIO_PIN_7
#else
/* 与本工程 Core/SRC/i2c.c 里 mcustudio 配的 I2C1 引脚一致（AF6、开漏） */
#define USBI_I2C_SCL_PORT GPIOA
#define USBI_I2C_SCL_PIN  GPIO_PIN_9
#define USBI_I2C_SDA_PORT GPIOA
#define USBI_I2C_SDA_PIN  GPIO_PIN_10
#endif

/* SCL 时钟频率。正版 USBi 固定 400 kHz，这里默认跟它一致。
 * ⚠ 400 kHz 需要板上 2 kΩ 上拉；如果只有 MCU 内部上拉（约 30~50 kΩ），
 *   上升沿跟不上，读回会变成 0xFF —— 那就先降到 100000 甚至 20000 排查。 */
#ifndef USBI_I2C_CLOCK_HZ
#define USBI_I2C_CLOCK_HZ 400000u
#endif

/* 单次等待（START / ADDR / TXE / BTF / 停止）的超时，单位 ms。
 * 248 字节一帧在 400 kHz 下约 5.6 ms，50 ms 够用；卡死时最多阻塞这么久。 */
#ifndef USBI_I2C_TIMEOUT_MS
#define USBI_I2C_TIMEOUT_MS 50u
#endif

/* 单次 I2C 事务（一个 START…STOP）允许的数据字节上限。
 * 数值取自 adau1452-esp32-controller 的实测值（它跑通过 1452 的完整下载）：
 *   - 核心寄存器区（>= 0xF000）：按字节寻址，一次 ≤ 24 字节
 *   - 程序内存区（0xC000~0xEFFF）：快路径 248 字节／保守路径一个 32 位字
 *   - 参数/DM 区：一次 ≤ 28 字节（保持 4 字节对齐）
 *   - 读方向放宽（减少事务数；240 是参考工程的实测上限） */
#define USBI_I2C_CHUNK_REG      24u
#define USBI_I2C_CHUNK_PARAM    28u
#define USBI_I2C_CHUNK_READ_REG 24u
#define USBI_I2C_CHUNK_READ_RAM 240u

/* 程序内存区走"快路径"（一次 248 字节）还是"保守路径"（一次一个 32 位字）。
 * 保守路径每写 4 字节就要 300 µs 间隔，12 KB 要 1.7 s、还把 USB 中断占住
 * 好几十毫秒；快路径把整段写完之后只等一次 300 µs settle，快 5 倍以上。 */
#ifndef USBI_I2C_FAST_PROG
#define USBI_I2C_FAST_PROG 1
#endif

#if USBI_I2C_FAST_PROG
#define USBI_I2C_CHUNK_PROG 248u
#else
#define USBI_I2C_CHUNK_PROG 4u
#endif

/* 单次事务失败后的重试次数（每次重试前先做总线恢复） */
#ifndef USBI_I2C_RETRIES
#define USBI_I2C_RETRIES 3u
#endif

/* ADAU145x 的 I2C 从机地址范围：0x38~0x3B
 *   地址由 SS/ADDR0 与 MOSI/ADDR1 引脚硬接线决定，两脚内部 250 kΩ 上拉，
 *   因此悬空 = 0x3B（最常用，参考工程也是 0x3B），任一脚下拉则变成 0x3A/0x39/0x38 */
#define USBI_I2C_ADDR_MIN     0x38u
#define USBI_I2C_ADDR_MAX     0x3Bu
#define USBI_I2C_ADDR_DEFAULT USBI_I2C_ADDR_MAX

/* 上电探测：每个地址试几次 */
#define USBI_I2C_PROBE_TRIALS 2u

/* 程序内存区每写完一块之后的等待。
 * 手册说批量下载不需要延长间隔（先 HIBERNATE 停核再块写即可），但
 * adau1452-esp32-controller 实测未加延时会"有 ACK 但指令没进 DSP → 无声"，
 * 这里默认沿用它的 300 µs；确认不需要可以改成 0 关掉 */
#ifndef USBI_I2C_PROG_WORD_DELAY_US
#define USBI_I2C_PROG_WORD_DELAY_US 300u
#endif

/* 地址区边界：>= 0xF000 是控制寄存器（2 字节宽、按字节寻址），
 * 0xC000~0xEFFF 是程序 RAM（4 字节宽、子地址按字自增），
 * 其余 < 0xC000 是参数/DM RAM（同样 4 字节宽） */
#define USBI_ADDR_REG_BASE  0xF000u
#define USBI_ADDR_PROG_BASE 0xC000u

/* ========================= USBi 协议常量 ========================= */
/* 官方 USBi 固件只用 EP0 上的这五个 vendor 请求（bRequest） */
#define USBI_REQ_WRITE_REGISTER             0xB2u /* 写：wLength = 2 + N，数据 = 寄存器号(大端2字节) + N 字节 */
#define USBI_REQ_READ_REGISTER              0xB3u /* 读请求：wLength = 2，数据 = 寄存器号(大端2字节) */
#define USBI_REQ_READ_REGISTER_RESPONSE     0xB4u /* 读响应：IN，返回 wLength 字节 */
#define USBI_REQ_READ_REGISTER_ACK          0xB5u /* ACK，IN，返回 0x01 */
#define USBI_REQ_READ_REGISTER_RESPONSE_ACK 0xB6u /* ACK，IN，返回 0x01 */

/* ACK 请求的返回值 */
#define USBI_ACK_BYTE 0x01u

/* ========================= USB 标识 ========================= */
/* 必须与 SigmaStudio 的 CyUSB.inf 匹配，否则 Windows 不会把设备绑到 CyUSB.sys：
 *   %VID_0456&PID_7031.DeviceDesc%=CyUsb ...  "Analog Devices USBi (programmed)"
 * 7031 = 已编程态；7030 是空片态，INF 会给它挂 DriverEXECSCRIPT 灌 ADI_USBi.spt，
 * 因此这里绝不能使用 7030。 */
#define USBI_VID 0x0456u
#define USBI_PID 0x7031u

/* 单次 EP0 控制传输（OUT/写请求）允许的最大字节数。
 * 必须与 usb_config.h 里的 CONFIG_USBDEV_REQUEST_BUFFER_LEN 相同：
 * CherryUSB 对 OUT 方向会先检查 wLength 是否超过该宏，超过就直接 STALL，
 * 那会让 SigmaStudio 下载程序时半路失败。 */
#define USBI_EP0_MAX_XFER  4096u

/* 0xB4 读响应的缓冲大小（实际读请求都很小，留 1 KB 足够） */
#define USBI_READ_BUF_SIZE 1024u

/* EP0 OUT 大块写的流式接收分块大小（必须是 4 的倍数：存储器按 32 位字寻址）。
 * 详见 usbi.c 里 usbi_ep0_out_stream() 的说明：官方 USBi 的写请求最长可到十几 KB
 * （烧 EEPROM 时 SigmaStudio 一次把 12568 字节的镜像写进 DM0），整块放进 16 KB SRAM
 * 是不可能的，所以按块边走边发。 */
#define USBI_EP0_STREAM_CHUNK 256u

/* ========================= RESET 输出 ========================= */
/* RESET 线接在 PB0（gpio.c 里配置成推挽输出）。空闲输出高 = 释放复位，
 * 需要复位时拉低 USBI_RESET_PULSE_MS 再释放。设计原则：**固件不主动复位目标**，
 * 复位只在主机明确要求时发生（理由见下面"烧录识别 / 会话结束识别"一段）。 */
#define USBI_RESET_GPIO_PORT     GPIOB
#define USBI_RESET_GPIO_PIN      GPIO_PIN_0
#define USBI_RESET_ASSERT_LEVEL  GPIO_PIN_RESET /* 低电平 = 复位有效 */
#define USBI_RESET_IDLE_LEVEL    GPIO_PIN_SET   /* 平时输出高 = 释放复位 */

/* RST 总开关（V1.0 = 1：常态推挽输出高，固件不主动拉低）
 *   0 = 完全不使用 RST（PB0 高阻，交给目标板自己的复位电路）
 *   1 = 空闲推挽输出高、需要复位时拉低
 * 用推挽输出而不是高阻：引脚高阻时复位线上只剩目标板自己的弱上拉，SPI 串扰
 * 就足以把它拉到复位门限以下。gpio.c 里 PB0 的配置跟随这个宏。 */
#ifndef USBI_RESET_ENABLE
#define USBI_RESET_ENABLE 1
#endif

/* 复位时序（毫秒），只在 USBI_RESET_ENABLE = 1 时有效 */
#define USBI_RESET_PULSE_MS     20u   /* 拉低（复位有效）的时长 */
#define USBI_RESET_RELEASE_MS   100u  /* 释放后等目标启动（PLL 锁定最大 10.7 ms） */

/* 上电是否先做一次复位脉冲。默认不做（PB0 上电即输出高，目标正常启动）。
 * 固件**不会**因为"主机在下载/烧录"而自动复位目标 —— 理由见下面一段。 */
#ifndef USBI_RESET_BOOT_PULSE
#define USBI_RESET_BOOT_PULSE 0
#endif

/* 主机侧复位通道（占位实现）：官方 USBi 的 RESET 由哪个 vendor 请求驱动目前
 * 未知（五个已知请求码里没有）。打开后"写寄存器 0xFFFF"被当作复位控制，
 * 数据第 1 字节非零 = 复位。等 USBPcap 抓到 SigmaStudio 的实际报文后再定。 */
#ifndef USBI_RESET_MAGIC_REG_ENABLE
#define USBI_RESET_MAGIC_REG_ENABLE 0
#endif
#ifndef USBI_RESET_MAGIC_REG
#define USBI_RESET_MAGIC_REG 0xFFFFu
#endif

/* ==================== 烧录识别 / 会话结束识别 ==================== */
/* 这两个识别都**只用于指示灯**，不触发任何复位。
 *
 * 依据实机抓包（ADAU1466 + SigmaStudio 4.7 的一次完整会话）：
 *   ① 烧 EEPROM 由 DSP 自己写：主机先把整幅镜像写进目标存储器
 *      （`DM0 @0x0000` 约 12 KB）并下载自举程序（`Program @0xC000`），再启动
 *      内核、写控制块 0x0028~0x003A，然后 `g_Erase(0x0035)` 等擦除，最后逐页把
 *      256 字节写到 0x0040（11 页，每页约 200 ms）。
 *      → "正在烧录"的判据 = 出现地址 < 0x0100 的大块写。
 *   ② 下载并启动设计：`Program @0xC000` + `DM0 @0x0000` + `DM1 @0x6000` 之后
 *      跟一组内核控制寄存器写
 *      `0xF404 START_ADDRESS` → `0xF403 KILL_CORE` → `0xF402 START_CORE`
 *      → `0xF401 START_PULSE` → [Delay] → `0xF400 HIBERNATE`。
 *      → 看到这组写 = 主机已经把 DSP 跑起来了，此后**绝不能再动 RST**。
 *
 * ★ 教训：旧版本用"800 ms 没有新的下载块就复位目标"当判据，而那次会话的最后
 *   一次大块写恰好落在 SigmaStudio 启动 DSP 之后 0.8 s，刚跑起来的程序被复位
 *   冲掉，实机表现就是"烧录完成后 DSP 不出声"。 */
#define USBI_BURN_PAGE_REG      0x0040u   /* 自举烧录器的 EEPROM 页数据寄存器 */
#define USBI_BURN_PAGE_MIN_LEN  128u      /* 页写是 256 字节 */
#define USBI_BURN_IMAGE_MIN_LEN 1024u     /* 整幅镜像写在 0x0000，12568 字节 */
#define USBI_BURN_IDLE_MS       1500u     /* 这么久没有烧录活动 → 只收掉烧录灯，不动 RST */

/* 识别到"主机启动 DSP"（0xF403 → 0xF402 → 0xF401）之后是否补一个复位脉冲。
 * 默认关闭：目标此刻正在跑主机刚下载的程序，复位会让它立刻安静下来。
 * 只有在想验证"EEPROM 自举"时才打开（复位后目标会从自己的 EEPROM 重新自举）。 */
#ifndef USBI_RESET_ON_SESSION_END
#define USBI_RESET_ON_SESSION_END 0
#endif

/* 启动序列识别用的内核控制寄存器（0xF400~0xF404） */
#define USBI_REG_HIBERNATE   0xF400u
#define USBI_REG_START_PULSE 0xF401u
#define USBI_REG_START_CORE  0xF402u
#define USBI_REG_KILL_CORE   0xF403u
#define USBI_REG_START_ADDR  0xF404u

/* Exported functions --------------------------------------------------------*/

/* 注册描述符 / vendor 接口并启动 USB 设备控制器（由 Studio_USB_Init() 调用） */
void usbi_usb_init(void);

/* 初始化目标总线（片选引脚、总线探测）。必须在
 * Studio_SPI1_Init() / Studio_I2C1_Init() 之后调用。 */
void usbi_init(void);

/* 主循环任务：处理"烧录灯超时收灯"等需要毫秒计时、不能在中断里做的事。
 * 注意：**不再**在这里自动复位目标（见上文"会话结束识别"）。 */
void usbi_task(void);

/* 目标 RESET 控制：assert != 0 → 复位有效（输出低）；0 → 释放（输出高） */
void usbi_target_reset(int assert);

/* 复位脉冲：输出低 USBI_RESET_PULSE_MS → 输出高，再等 USBI_RESET_RELEASE_MS。
 * 只能在主循环/初始化阶段调用（内部用 HAL_Delay，需要 SysTick 在跑）。 */
void usbi_target_reset_pulse(void);

/* 当前生效的总线（USBI_BUS_SPI / USBI_BUS_I2C） */
uint8_t usbi_bus_get(void);

/* 运行时切换总线。切到 I2C 会先给目标一个复位脉冲（否则目标不会回到 I2C
 * 模式）；切到 SPI 不主动发脉冲，交给 SigmaStudio 自己的 3 次 SPI Toggle。 */
void usbi_bus_set(uint8_t bus);

/* 当前是否判定为"主机正在烧录 EEPROM"（只用于指示灯，不触发复位） */
uint8_t usbi_flash_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __USBI_H__ */
