/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbi.c
  * @brief   Analog Devices USBi (EVAL-ADUSB2EBZ) 兼容仿真器 —— 协议与总线实现
  *
  *  组成：
  *    1) USB 描述符：VID 0x0456 / PID 0x7031、单配置单接口、bInterfaceClass=0xFF、
  *       bNumEndpoints=0 —— 与官方 USBi 对外呈现的形状完全一致（没有 bulk/
  *       中断端点，SigmaStudio 只用 EP0 的 vendor 请求）。
  *    2) vendor_handler：实现 0xB2 写寄存器 / 0xB3 读请求 / 0xB4 读响应 /
  *       0xB5、0xB6 ACK；未知请求一律让 core STALL（绝不能 panic 或死循环）。
  *    3) 总线后端：SPI（片选 PA4 由软件控制，见 spi_io_init）或 I2C（硬件
  *       I2C1 + HAL，400 kHz，PA9=SCL / PA10=SDA；引脚/时钟/分块大小见 usbi.h
  *       的 USBI_BUS_SELECT 与 USBI_I2C_* 宏）。
  *       两档共用同一套 vendor 请求处理，切换只需改宏重新编译。
  *    4) RESET 输出（PB0）：常态推挽输出高（释放复位）；固件不主动复位目标。
  *    5) 烧录识别 / "主机启动 DSP"序列识别：**只驱动指示灯**，不触发任何复位。
  *
  *  参考实现与手册依据：
  *    - rp2040-Analog-Devices-USBi（Rust/embassy，真正的 USB USBi 仿真器）
  *    - adau1452-esp32-controller（I2C 分块/延时、ADAU145x 地址语义）
  *    - ADAU1452/1451/1450 数据手册 Rev.D：P30/P32/P42/P43/P44/P47/P93
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbi.h"
#include "usb_config.h"
#include "usbd_core.h"
#include "usbi_led.h"
#include "spi.h"
#include "i2c.h"
#include <string.h>

/* Private define ------------------------------------------------------------*/
/* 配置描述符总长 = 9（配置）+ 9（接口），无端点描述符 */
#define USBI_CONFIG_DESC_SIZ 18u

/* bMaxPower = 100 mA（走 USB_CONFIG_POWER_MA() 折半写入，即 0x32） */
#define USBI_MAX_POWER 100u

/* Private variables ---------------------------------------------------------*/
/**
  * @brief 设备描述符集合
  *
  * 注意 iManufacturer / iProduct / iSerial 由 USB_DEVICE_DESCRIPTOR_INIT()
  * 固定写成 1 / 2 / 3，因此下面三个字符串必须按顺序全部提供。
  */
static const uint8_t usbi_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBI_VID, USBI_PID, 0x0002, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USBI_CONFIG_DESC_SIZ, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBI_MAX_POWER),

    /* ---- 接口描述符：vendor specific、零端点（官方 USBi 就是这样） ---- */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, /* bInterfaceNumber */
                                  0x00, /* bAlternateSetting */
                                  0x00, /* bNumEndpoints = 0 */
                                  0xFF, /* bInterfaceClass = vendor specific */
                                  0x00, /* bInterfaceSubClass */
                                  0x00, /* bInterfaceProtocol */
                                  0x00),/* iInterface */

    /* ---- string0：语言 ID ---- */
    USB_LANGID_INIT(0x0409),

    /* ---- string1："Analog Devices" ---- */
    0x1e, USB_DESCRIPTOR_TYPE_STRING,
    'A', 0x00, 'n', 0x00, 'a', 0x00, 'l', 0x00, 'o', 0x00, 'g', 0x00, ' ', 0x00,
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 's', 0x00,

    /* ---- string2："USBi" ---- */
    0x0a, USB_DESCRIPTOR_TYPE_STRING,
    'U', 0x00, 'S', 0x00, 'B', 0x00, 'i', 0x00,

    /* ---- string3："0001"（序列号，仅为满足 iSerial 索引） ---- */
    0x0a, USB_DESCRIPTOR_TYPE_STRING,
    '0', 0x00, '0', 0x00, '0', 0x00, '1', 0x00,

    /* 描述符表结束标记（CherryUSB 按 bLength 逐条遍历） */
    0x00
};

/* vendor 接口对象；intf_num 由 usbd_add_interface() 自动填写 */
static struct usbd_interface usbi_intf;

/* 读请求（0xB3）登记下来的寄存器号，等 0xB4 时真正去读总线 */
static uint16_t s_pending_read_reg;
static volatile uint8_t s_pending_read_valid;

/* 0xB4 的返回缓冲。必须 4 字节对齐，否则 CherryUSB 会把数据拷进只有
 * CONFIG_USBDEV_REQUEST_BUFFER_LEN 字节的 req_data（usbd_core.c:850-859）。 */
static uint8_t s_read_buf[USBI_READ_BUF_SIZE] __attribute__((aligned(4)));

/* 0xB5 / 0xB6 返回的"上一次传输状态"：ADIUSBi.dll 的 TransferStatus，
 * 1 = Success、3 = Failed（抓包与 adau1452-esp32-controller 一致）。
 * SPI 是开环的，失败极少；I2C 有 ACK，总线 NACK 必须如实报上去，否则 SigmaStudio
 * 会把一次写失败的下载当成成功。 */
#define USBI_ST_SUCCESS 1u
#define USBI_ST_FAILED  3u

static uint8_t s_write_status = USBI_ST_SUCCESS;
static uint8_t s_read_status  = USBI_ST_SUCCESS;

/* 总线是否已初始化（SPI/I2C 初始化在 USB 初始化之后） */
static volatile uint8_t s_bus_ready;

/* 当前生效的总线（USBI_BUS_SPI / USBI_BUS_I2C），由 usbi_bus_set() 决定 */
static volatile uint8_t s_bus = USBI_BUS_SPI;

/* ---- 烧录 / 会话结束识别状态（USB 中断里更新，主循环里消费） ----
 * s_burn_active      : 主机正在烧 EEPROM（整幅镜像或 256 字节页数据）→ 点亮烧录灯
 * s_boot_step        : "主机启动 DSP" 序列 0xF403 → 0xF402 → 0xF401 的识别进度
 * s_session_started  : 已经看到启动序列（主机把 DSP 跑起来了），由 usbi_task() 消费
 * 说明：这些状态**不触发复位**，只影响指示灯，详见 usbi.h 里的一整段说明。 */
static volatile uint32_t s_burn_last_ms;
static volatile uint8_t  s_burn_active;
static volatile uint8_t  s_boot_step;
static volatile uint8_t  s_session_started;

/* ---- 超长块写（0xB2）的流式接收状态 ----
 * 官方 USBi 的一次写请求最长能到十几 KB（烧 EEPROM 时 SigmaStudio 把 12568 字节
 * 的整幅镜像一次写进 DM0）。整个缓冲放不下，所以按块边走边发到目标总线。
 * s_stream_buf 交给 CherryUSB 填，s_stream_stage 用来凑够 4 字节整字再发。 */
static uint8_t  s_stream_buf[USBI_EP0_STREAM_CHUNK + 4u];
static uint8_t  s_stream_stage[USBI_EP0_STREAM_CHUNK + 4u];
static uint16_t s_stream_reg;            /* 本次块写的寄存器号 */
static uint32_t s_stream_data_written;   /* 已经发给目标的数据字节数（不含 2 字节寄存器号） */
static uint8_t  s_stream_stage_len;      /* 暂存区里还没凑成整字的字节数（0~3） */

/* Private function prototypes -----------------------------------------------*/
static int  usbi_vendor_request(struct usb_setup_packet *setup, uint8_t **data, uint32_t *len);
static void usbi_notify_handler(uint8_t event, void *arg);

static int  usbi_ep0_out_stream(struct usb_setup_packet *setup, uint8_t *data,
                                uint32_t len, uint32_t offset, uint32_t total);

static int  bus_write(uint16_t reg, const uint8_t *data, uint16_t len);
static int  bus_read(uint16_t reg, uint8_t *data, uint16_t len);
static int  bus_write_at(uint16_t reg, uint16_t byte_off, const uint8_t *data, uint16_t len);

static void reset_pin_idle(void);
static void reset_pin_assert(void);

static void spi_io_init(void);
static void spi_cs(int active);

/* I2C 后端（硬件 I2C1 + HAL）的函数定义在"总线分发"之前，这里不需要额外原型 */

static void flash_track(uint16_t reg, uint16_t len);

/* =========================== RESET 输出 =========================== */
#if USBI_RESET_ENABLE
/**
  * @brief 把 RESET 引脚置为"释放"状态：推挽输出高电平
  *
  * 用推挽输出而不是"输入 + 上拉"（高阻）：引脚高阻时 RST 上只剩目标板自己的
  * 弱上拉，SPI 串扰就足以把它拉到复位门限以下，实机上表现为"在线调音的
  * safeload 也会把 DSP 复位掉"。推挽高是确定性的。
  */
static void reset_pin_idle(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 先写 ODR 再切模式，避免切换瞬间出现毛刺 */
    HAL_GPIO_WritePin(USBI_RESET_GPIO_PORT, USBI_RESET_GPIO_PIN, USBI_RESET_IDLE_LEVEL);
    gpio.Pin = USBI_RESET_GPIO_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(USBI_RESET_GPIO_PORT, &gpio);
    HAL_GPIO_WritePin(USBI_RESET_GPIO_PORT, USBI_RESET_GPIO_PIN, USBI_RESET_IDLE_LEVEL);
}

/**
  * @brief 把 RESET 引脚拉低（复位有效）
  */
static void reset_pin_assert(void)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(USBI_RESET_GPIO_PORT, USBI_RESET_GPIO_PIN, USBI_RESET_ASSERT_LEVEL);
    gpio.Pin = USBI_RESET_GPIO_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(USBI_RESET_GPIO_PORT, &gpio);
}
#else  /* USBI_RESET_ENABLE == 0：固件完全不碰复位线 */
/**
  * @brief 把 RESET 引脚置成高阻（模拟输入）——"不使用 RST"
  *
  * 既不驱动低也不驱动高，目标板的复位线完全由它自己的复位电路控制。
  * 反复调用是幂等的（HAL_GPIO_Init 只写寄存器）。
  */
static void reset_pin_idle(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = USBI_RESET_GPIO_PIN;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(USBI_RESET_GPIO_PORT, &gpio);
}

/**
  * @brief 空实现：本模式下固件永不复位目标
  */
static void reset_pin_assert(void)
{
}
#endif /* USBI_RESET_ENABLE */

void usbi_target_reset(int assert)
{
    if (assert) {
        reset_pin_assert();
    } else {
        reset_pin_idle();
    }
}

/**
  * @brief 复位脉冲：输出低 USBI_RESET_PULSE_MS → 输出高，再等目标启动
  *
  * 只在主循环/初始化阶段调用（内部用了 HAL_Delay，需要 SysTick 在跑）。
  *
  * USBI_RESET_ENABLE = 0 时本函数不做任何复位动作（连延时都跳过），只把 PB0
  * 确保成高阻；= 1（V1.0）时按上面的时序真正拉低一次复位。然后同样置 s_bus_ready。
  */
void usbi_target_reset_pulse(void)
{
#if USBI_RESET_ENABLE
    reset_pin_assert();
    HAL_Delay(USBI_RESET_PULSE_MS);
    reset_pin_idle();
    HAL_Delay(USBI_RESET_RELEASE_MS);   /* 控制口 650 ns 就绪，PLL 锁定最大 10.7 ms */
#else
    reset_pin_idle();                   /* 幂等：确保 PB0 保持高阻 */
#endif

    s_bus_ready = 1u;
}

/* =========================== SPI 后端 =========================== */
/**
  * @brief 片选：ADAU 要求事务期间低、其余时间高
  */
static void spi_cs(int active)
{
    HAL_GPIO_WritePin(USBI_SPI_CS_PORT, USBI_SPI_CS_PIN,
                      active ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/**
  * @brief  把 SPI1 的外设与引脚准备成"软件片选"的状态
  */
static void spi_io_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* ① PA4 从 SPI1_NSS 复用收回，改由软件精确控制片选。
     * mcustudio 生成的是 SPI_NSS_HARD_OUTPUT，硬件 NSS 做不出 ADAU 需要的
     * 片选边界（一次事务一个完整的低-高脉冲）。 */
    HAL_GPIO_DeInit(USBI_SPI_CS_PORT, USBI_SPI_CS_PIN);
    gpio.Pin = USBI_SPI_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(USBI_SPI_CS_PORT, &gpio);
    spi_cs(0);

    /* ② SPI1 切到软件 NSS（SSM|SSI=1）并关闭 SSOE。
     * SSI 必须为 1，否则主机模式会因 NSS 被拉低而报 MODF。 */
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 |= (SPI_CR1_SSM | SPI_CR1_SSI);
    SPI1->CR2 &= ~SPI_CR2_SSOE;
    SPI1->CR1 |= SPI_CR1_SPE;
}

/**
  * @brief SPI 写：`00 <ah> <al> <data…>`（7 位 chip address = 0 + R/W=0，MSB first）
  */
static int bus_spi_write(uint16_t reg, const uint8_t *data, uint16_t len)
{
    uint8_t hdr[3];

    hdr[0] = 0x00;                    /* 写命令 */
    hdr[1] = (uint8_t)(reg >> 8);     /* 子地址高字节 */
    hdr[2] = (uint8_t)(reg & 0xFFu);  /* 子地址低字节 */

    spi_cs(1);
    if (HAL_SPI_Transmit(&hspi1, hdr, 3, HAL_MAX_DELAY) != HAL_OK) {
        spi_cs(0);
        return -1;
    }
    if (len) {
        if (HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, HAL_MAX_DELAY) != HAL_OK) {
            spi_cs(0);
            return -1;
        }
    }
    spi_cs(0);
    return 0;
}

/**
  * @brief SPI 读：`01 <ah> <al>` + 读 N 字节
  */
static int bus_spi_read(uint16_t reg, uint8_t *data, uint16_t len)
{
    uint8_t hdr[3];

    hdr[0] = 0x01;                    /* 读命令 */
    hdr[1] = (uint8_t)(reg >> 8);
    hdr[2] = (uint8_t)(reg & 0xFFu);

    spi_cs(1);
    if (HAL_SPI_Transmit(&hspi1, hdr, 3, HAL_MAX_DELAY) != HAL_OK) {
        spi_cs(0);
        return -1;
    }
    if (len) {
        /* 全双工模式下 HAL_SPI_Receive 会自动发哑元字节，ADAU 在读周期忽略
         * MOSI，因此不影响 */
        if (HAL_SPI_Receive(&hspi1, data, len, HAL_MAX_DELAY) != HAL_OK) {
            spi_cs(0);
            return -1;
        }
    }
    spi_cs(0);
    return 0;
}

/* ========================= I2C 后端（硬件 I2C1 + HAL） =========================
 * 正版 USBi 的 DSP 侧 I2C 固定 400 kHz，所以这里直接用硬件 I2C1：
 *   SCL = PA9、SDA = PA10（AF6、开漏、内部上拉），配置在 Core/SRC/i2c.c 里。
 *
 * ★★ 前提条件（改这里之前先看懂）★★
 *   vendor 请求全部在 USB 中断里执行，而 HAL 的每一处等待都是
 *       while (!flag) { if (HAL_GetTick() - tickstart > Timeout) 报错; }
 *   SysTick 是 HAL_GetTick() 的唯一时基。如果 SysTick 的优先级低于 USB 中断，
 *   USB 中断期间 uwTick 不前进 → 超时判断永远为假 → 总线一旦被从机拉死，
 *   中断就永久卡在 while 里，只能等 IWDG 把整机复位、USB 重枚举。
 *   因此本工程做了两件事（缺一不可）：
 *     1) py32f071_hal_conf.h: TICK_INT_PRIORITY = 0（SysTick 最高优先级）
 *     2) Core/SRC/usb.c: USB_IRQn 的优先级 = 1（让 SysTick 能抢占它）
 *   这样 HAL 的超时在中断里是真生效的：最坏情况是等 USBI_I2C_TIMEOUT_MS
 *   之后返回 HAL_ERROR，中断照常退出，下一次请求还能重来。
 *
 * ★ 出错恢复：HAL 传输失败后 hi2c1.State 会停在 BUSY_TX / BUSY_RX，
 *   之后所有调用都直接返回 HAL_BUSY（连 START 都不会发）。所以每次失败都
 *   必须 DeInit/Init，并把 9 个时钟打到总线上把从机卡住的 SDA 顶出来。
 *
 * 分块、子地址推进、程序区 300 µs settle 与参考工程 adau1452-esp32-controller
 * 完全一致，只把"比特怎么打"从软件模拟换成了外设。
 */

/* 7 位从机地址（0x38~0x3B，ADAU145x 由 ADDR0/ADDR1 决定） */
static uint8_t s_i2c_addr = USBI_I2C_ADDR_DEFAULT;

/* 引脚：正常传输时由 I2C1 外设占用，只在"总线恢复"时临时当 GPIO 用 */
#define I2C_SCL_PORT USBI_I2C_SCL_PORT
#define I2C_SCL_PIN  USBI_I2C_SCL_PIN
#define I2C_SDA_PORT USBI_I2C_SDA_PORT
#define I2C_SDA_PIN  USBI_I2C_SDA_PIN

#define I2C_SCL_HIGH() (I2C_SCL_PORT->BSRR = (uint32_t)I2C_SCL_PIN)
#define I2C_SCL_LOW()  (I2C_SCL_PORT->BSRR = (uint32_t)I2C_SCL_PIN << 16)
#define I2C_SDA_HIGH() (I2C_SDA_PORT->BSRR = (uint32_t)I2C_SDA_PIN)
#define I2C_SDA_LOW()  (I2C_SDA_PORT->BSRR = (uint32_t)I2C_SDA_PIN << 16)
#define I2C_SDA_LEVEL() (((I2C_SDA_PORT->IDR & I2C_SDA_PIN) != 0u) ? 1u : 0u)

/**
  * @brief 忙等若干微秒（用 SysTick 计数器，**在中断里也能用**）
  *
  * 不用 HAL_Delay：它依赖中断里的 uwTick，而这种等待都发生在 USB 中断里。
  * SysTick->VAL 是硬件计数器，CPU 在中断里它照样在跑。
  * 单次最多一个 SysTick 周期（1 ms），够 300 µs 的 settle 用。
  */
static void i2c_delay_us(uint32_t us)
{
    uint32_t load = SysTick->LOAD + 1u;
    uint32_t total = us * (SystemCoreClock / 1000000u);
    uint32_t start;
    uint32_t now;
    uint32_t elapsed;

    if (total == 0u) {
        return;
    }
    if (total >= load) {
        total = load - 1u;              /* 夹到一个 SysTick 周期内，保证有界 */
    }
    start = SysTick->VAL;
    do {
        now = SysTick->VAL;
        elapsed = (start >= now) ? (start - now) : (start + load - now);
    } while (elapsed < total);
}

/**
  * @brief 把 SCL/SDA 从 I2C1 外设手上夺过来，改成开漏 GPIO
  */
static void i2c_pins_as_gpio(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_I2C_DISABLE(&hi2c1);

    HAL_GPIO_DeInit(I2C_SCL_PORT, I2C_SCL_PIN);
    HAL_GPIO_DeInit(I2C_SDA_PORT, I2C_SDA_PIN);

    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = I2C_SCL_PIN;
    HAL_GPIO_Init(I2C_SCL_PORT, &gpio);
    gpio.Pin = I2C_SDA_PIN;
    HAL_GPIO_Init(I2C_SDA_PORT, &gpio);

    I2C_SCL_HIGH();
    I2C_SDA_HIGH();
}

/**
  * @brief 总线恢复 + 重新初始化 I2C1
  *
  * 从机在半个字节中间被复位/断电时会把 SDA 拉低不放，这时主机发的 START
  * 永远等不到（硬件上表现为 BUSY 一直置位）。标准解法是补最多 9 个时钟，
  * 让它把剩下的数据位吐完、放开 SDA，再补一个 STOP。
  * 全程有上界（9 个时钟 / 每次 5 µs），不会死等。
  */
static void i2c_bus_recover(void)
{
    uint8_t i;

    i2c_pins_as_gpio();
    i2c_delay_us(5);

    for (i = 0u; (i < 9u) && (I2C_SDA_LEVEL() == 0u); i++) {
        I2C_SCL_LOW();
        i2c_delay_us(5);
        I2C_SCL_HIGH();
        i2c_delay_us(5);
    }

    /* 补一个 STOP：SCL 低 → SDA 低 → SCL 高 → SDA 高 */
    I2C_SCL_LOW();
    i2c_delay_us(5);
    I2C_SDA_LOW();
    i2c_delay_us(5);
    I2C_SCL_HIGH();
    i2c_delay_us(5);
    I2C_SDA_HIGH();
    i2c_delay_us(5);

    /* 引脚交还给 I2C1（MspInit 里会重新配成 AF6 开漏并复位外设） */
    (void)HAL_I2C_DeInit(&hi2c1);
    hi2c1.Init.ClockSpeed = USBI_I2C_CLOCK_HZ;
    (void)HAL_I2C_Init(&hi2c1);
}

/**
  * @brief 带重试的 I2C 事务（每失败一次就做总线恢复再重来）
  *
  * @param write  1 = 写，0 = 读
  * @param sub    16 位子地址（大端先发，HAL 的 MEMADD_SIZE_16BIT 负责）
  * @retval 0 = 成功；-1 = 重试用尽仍失败
  */
static int i2c_txn(uint8_t write, uint16_t sub, uint8_t *data, uint16_t len)
{
    uint8_t tries = USBI_I2C_RETRIES;
    uint16_t dev = (uint16_t)((uint16_t)s_i2c_addr << 1);

    do {
        HAL_StatusTypeDef st;

        if (write != 0u) {
            st = HAL_I2C_Mem_Write(&hi2c1, dev, sub, I2C_MEMADD_SIZE_16BIT,
                                   data, len, USBI_I2C_TIMEOUT_MS);
        } else {
            st = HAL_I2C_Mem_Read(&hi2c1, dev, sub, I2C_MEMADD_SIZE_16BIT,
                                  data, len, USBI_I2C_TIMEOUT_MS);
        }
        if (st == HAL_OK) {
            return 0;
        }
        i2c_bus_recover();
        tries = (uint8_t)(tries - 1u);
    } while (tries != 0u);

    return -1;
}

/**
  * @brief 在 I2C 总线上探测 ADAU（地址 0x38~0x3B）
  * @retval 0 = 找到，*addr_out 为 7 位地址；-1 = 没找到
  */
static int i2c_probe(uint8_t *addr_out)
{
    uint8_t addr;
    uint8_t trial;

    for (addr = USBI_I2C_ADDR_MIN; addr <= USBI_I2C_ADDR_MAX; addr++) {
        for (trial = 0u; trial < USBI_I2C_PROBE_TRIALS; trial++) {
            if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)((uint16_t)addr << 1),
                                      1u, USBI_I2C_TIMEOUT_MS) == HAL_OK) {
                *addr_out = addr;
                return 0;
            }
            i2c_bus_recover();
        }
    }
    return -1;
}

/**
  * @brief 子地址自增规则
  *
  * 手册 P43：控制寄存器是 2 字节宽、存储器是 4 字节宽，子地址按"字"自动自增。
  * 因此 >= 0xF000 的寄存器区按字节推进，其余按 4 字节为一个子地址。
  */
static uint16_t i2c_next_sub(uint16_t sub, uint16_t bytes)
{
    if (sub >= USBI_ADDR_REG_BASE) {
        return (uint16_t)(sub + bytes);
    }
    return (uint16_t)(sub + (bytes / 4u));
}

/**
  * @brief 单次 I2C 事务允许的数据字节数
  *
  * 与 adau1452-esp32-controller 的实测值一致：寄存器区 ≤24、程序区快路径
  * 248（保守路径一个 32 位字）、参数区 ≤28（4 字节对齐）、读方向 240。
  */
static uint16_t i2c_chunk(uint16_t sub, uint16_t len, uint8_t read)
{
    uint16_t limit;

    if (sub >= USBI_ADDR_REG_BASE) {
        limit = (read != 0u) ? USBI_I2C_CHUNK_READ_REG : USBI_I2C_CHUNK_REG;
    } else if (sub >= USBI_ADDR_PROG_BASE) {
        limit = (read != 0u) ? USBI_I2C_CHUNK_READ_RAM : USBI_I2C_CHUNK_PROG;
    } else {
        limit = (read != 0u) ? USBI_I2C_CHUNK_READ_RAM : USBI_I2C_CHUNK_PARAM;
    }
    if (limit == 0u) {
        limit = 1u;
    }
    return (len > limit) ? limit : len;
}

/**
  * @brief I2C 写：按区域分块，每块 = 2 字节子地址 + 数据（一个完整事务）
  */
static int bus_i2c_write(uint16_t reg, const uint8_t *data, uint16_t len)
{
    uint16_t sub = reg;

    while (len != 0u) {
        uint16_t chunk = i2c_chunk(sub, len, 0u);
        uint8_t is_prog = (uint8_t)((sub >= USBI_ADDR_PROG_BASE) &&
                                    (sub < USBI_ADDR_REG_BASE));

        if (i2c_txn(1u, sub, (uint8_t *)data, chunk) != 0) {
            return -1;
        }

        sub = i2c_next_sub(sub, chunk);
        data += chunk;
        len = (uint16_t)(len - chunk);

        /* 程序内存区写完一块要停一下让 DSP 真正吃掉（参考工程的 300 µs） */
        if ((is_prog != 0u) && (len != 0u)) {
            i2c_delay_us(USBI_I2C_PROG_WORD_DELAY_US);
        }
    }
    return 0;
}

/**
  * @brief I2C 读：写子地址 + 重复 START + 读数据（HAL_I2C_Mem_Read 内部完成）
  */
static int bus_i2c_read(uint16_t reg, uint8_t *data, uint16_t len)
{
    uint16_t sub = reg;

    while (len != 0u) {
        uint16_t chunk = i2c_chunk(sub, len, 1u);

        if (i2c_txn(0u, sub, data, chunk) != 0) {
            return -1;
        }

        sub = i2c_next_sub(sub, chunk);
        data += chunk;
        len = (uint16_t)(len - chunk);
    }
    return 0;
}

/**
  * @brief 准备 I2C1 主机并探测目标
  *
  * 只在主循环上下文调用（usbi_init / usbi_bus_set）。
  *
  * @retval 0 = 探测到目标；-1 = 总线上没有响应（主机照样准备好，后续传输会失败）
  */
static int i2c_bus_setup(void)
{
    /* 上一次会话可能把 State 留在 BUSY 里，先复位再重配 */
    (void)HAL_I2C_DeInit(&hi2c1);
    hi2c1.Init.ClockSpeed = USBI_I2C_CLOCK_HZ;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        return -1;
    }

    /* 先清一次可能被拉住的总线（内含 DeInit/Init），再探测 */
    i2c_bus_recover();

    s_i2c_addr = USBI_I2C_ADDR_DEFAULT;
    return i2c_probe(&s_i2c_addr);
}

/* =========================== 总线分发 =========================== */
/* 上层（vendor 请求、块写流式转发）完全不关心底下是哪条总线，
 * 只有这两处按 s_bus 分叉。 */
static int bus_write(uint16_t reg, const uint8_t *data, uint16_t len)
{
    if (s_bus == USBI_BUS_I2C) {
        return bus_i2c_write(reg, data, len);
    }
    return bus_spi_write(reg, data, len);
}

static int bus_read(uint16_t reg, uint8_t *data, uint16_t len)
{
    if (s_bus == USBI_BUS_I2C) {
        return bus_i2c_read(reg, data, len);
    }
    return bus_spi_read(reg, data, len);
}

/**
  * @brief 从"块写数据流的第 byte_off 字节"开始写
  *
  * 大块写（例如烧 EEPROM 时那 12568 字节）会被 CherryUSB 的分块回调拆开，
  * 每一块都要换算回正确的子地址：
  *   - 控制寄存器区（>= 0xF000）：按字节推进（每次 +1）
  *   - 存储器区（< 0xF000）：按 4 字节字推进（每 4 字节 +1）
  * 与 i2c_next_sub() 的规则一致，也与主机侧的块写语义一致。
  */
static int bus_write_at(uint16_t reg, uint16_t byte_off, const uint8_t *data, uint16_t len)
{
    uint16_t sub = reg;

    if (byte_off) {
        if (reg >= USBI_ADDR_REG_BASE) {
            sub = (uint16_t)(reg + byte_off);
        } else {
            sub = (uint16_t)(reg + (byte_off / 4u));
        }
    }
    return bus_write(sub, data, len);
}

uint8_t usbi_bus_get(void)
{
    return s_bus;
}

/**
  * @brief 切换（或首次选定）目标总线
  *
  * **只能在主循环上下文调用**：切到 I2C 时要用 usbi_target_reset_pulse()，
  * 里面有 HAL_Delay()，在 USB 中断里会死等。
  *
  * 切到 SPI：只把片选/SPI 外设准备好即可。**不主动发脉冲切模式** ——
  *   SigmaStudio 每次下载都会自己连发 3 次 "SPI Toggle"（向 0x4FFF 写 4 字节
  *   0x00，中间夹 "SPI Toggle Delay"），每次 SPI 事务天然在 SS/ADDR0 上产生
  *   一个低-高脉冲，三次就把目标切进 SPI 模式了。
  * 切到 I2C：必须先硬件复位目标。ADAU145x 一旦进过 SPI 模式就再也回不到 I2C
  *   （手册明确软复位 0xF890 也不行），只有 RESET 引脚能救。
  *   USBI_BUS_AUTO 例外：探测时目标本来就好好答话，不必再复位它。
  */
void usbi_bus_set(uint8_t bus)
{
    uint8_t do_reset = 1u;

    if (bus == USBI_BUS_AUTO) {
        /* 先探测：I2C 上有从机答话就走 I2C，否则退回 SPI */
        if (i2c_bus_setup() == 0) {
            bus = USBI_BUS_I2C;
            do_reset = 0u;          /* 刚探测到，别再复位 */
        } else {
            bus = USBI_BUS_SPI;
        }
    }

    if (bus == USBI_BUS_I2C) {
        s_bus_ready = 0u;           /* 切换过程中别让 vendor 请求动总线 */
        if (do_reset != 0u) {
            usbi_target_reset_pulse();
        }
        /* SS/ADDR0 保持高：地址低 2 位 = 0b11（0x3B），也不给 SPI 切换脉冲 */
        spi_io_init();
        if (do_reset != 0u) {
            (void)i2c_bus_setup();  /* 探测不到也照样切：主机可能只是先把模式设好 */
        }
        s_bus = USBI_BUS_I2C;
    } else {
        spi_io_init();
        s_bus = USBI_BUS_SPI;
    }

    s_bus_ready = 1u;
    usbi_led_set_bus(s_bus);        /* 指示灯色相跟着走 */
}

/* ========================= 烧录 / 会话结束识别 ========================= */
uint8_t usbi_flash_active(void)
{
    return s_burn_active;
}

/**
  * @brief "主机启动 DSP"的寄存器序列识别：0xF403 → 0xF402 → 0xF401
  *
  * 抓包里的原样顺序（captureall.txt #215~#221，16:27:16.882~889）：
  *   START_ADDRESS(0xF404=0) → KILL_CORE(0xF403=0) → START_CORE(0xF402=1)
  *   → START_CORE(0xF402=0) → START_PULSE(0xF401=2) → [Start Delay] → HIBERNATE(0xF400=0)
  * 这里只认 0xF403 → 0xF402 → 0xF401 三步，中间允许 0xF400/0xF404 穿插；
  * 任何其它控制寄存器写都会把状态机打回起点，避免误触发。
  *
  * 识别到就说明主机已经把 DSP 跑起来了：结束烧录灯状态，并且**不复位目标**
  * （否则刚跑起来的程序会被打回停止）。
  */
static void boot_seq_track(uint16_t reg)
{
    if (reg == USBI_REG_KILL_CORE) {
        s_boot_step = 1u;
    } else if (reg == USBI_REG_START_CORE) {
        if (s_boot_step == 1u) {
            s_boot_step = 2u;
        }
    } else if (reg == USBI_REG_START_PULSE) {
        if (s_boot_step == 2u) {
            s_boot_step = 0u;
            s_session_started = 1u;
            s_burn_active = 0u;
            usbi_led_set_flashing(0u);
        }
    } else if ((reg != USBI_REG_HIBERNATE) && (reg != USBI_REG_START_ADDR)) {
        s_boot_step = 0u;
    }
}

/**
  * @brief 每次成功的 0xB2 写之后统计一次（在 USB 中断上下文里调用）
  *
  * 只做两件事，都不碰 RST：
  *   ① 识别"正在烧 EEPROM" → 点亮烧录灯（红色高频闪）；
  *      判据 = 地址 < 0x0100 的整幅镜像写（12568 字节 @0x0000）或页数据写
  *      （256 字节 @0x0040）。普通下载的 DM0/DM1 只有 528 字节、也不会写 0x0040；
  *      运行时在线调音（safeload）只有 4~20 字节 —— 都天然不会误判。
  *   ② 识别"主机启动 DSP"   → 收掉烧录灯，标记会话结束。
  */
static void flash_track(uint16_t reg, uint16_t len)
{
    /* ① 烧 EEPROM（整幅镜像 / EEPROM 页数据）—— 判据只用来**首次点亮**烧录灯 */
    if (((reg < 0x0100u) && (len >= USBI_BURN_IMAGE_MIN_LEN)) ||
        ((reg == USBI_BURN_PAGE_REG) && (len >= USBI_BURN_PAGE_MIN_LEN))) {
        if (!s_burn_active) {
            usbi_led_set_flashing(1u);
        }
        s_burn_active = 1u;
        s_burn_last_ms = HAL_GetTick();
        return;
    }

    /* ② 灯已经亮着：**任何**一次写都算烧录活动，续上超时窗口
     *
     *    原来只有大块写才续窗口（"整幅镜像 / 页数据"那两条判据），于是烧录
     *    过程中穿插的 2 字节 / 4 字节寄存器写完全不续 → 两条大块写之间一旦
     *    超过 1500 ms，灯就被 usbi_task() 收掉，再被下一条大块写重新点亮。
     *    用户看到的就是"只有 program data / dm0,dm1 data 会闪，单写寄存器
     *    没反应"。
     *
     *    这里只续写、不续读：读是主机的常态轮询（SigmaStudio 空闲时一直在
     *    读寄存器），若读也续窗口，烧录结束后轮询会把烧录色永久钉在灯上。
     *    读命令的"响应"由 usbi_led_notify_read() 的闪灯相位拉回负责。 */
    if (s_burn_active) {
        s_burn_last_ms = HAL_GetTick();
    }

    /* ③ 主机启动 DSP 的序列（只可能出现在内核控制寄存器区） */
    if (reg >= USBI_ADDR_REG_BASE) {
        boot_seq_track(reg);
    }
}

/* ======================= EP0 超长块写的流式接收 ======================= */
/**
  * @brief  CherryUSB 分块回调：超长块写（wLength > 4096）边走边发到目标总线
  *
  * 为什么需要它：烧 EEPROM 时 SigmaStudio 会一次写 12568 字节（DM0 整幅镜像，
  * 见 capture.txt）。CherryUSB 默认把整个请求收进 CONFIG_USBDEV_REQUEST_BUFFER_LEN
  * 的静态缓冲，超限就直接 STALL —— 这正是"写寄存器/回读/在线调音都正常、只有烧录
  * flash 失败"的原因。而 16 KB SRAM 也放不下整块，所以只能按 256 字节一块收、
  * 收一块就发一块。
  *
  * 请求布局：前 2 字节 = 寄存器号（大端），其后全部是数据。数据部分只按 4 字节
  * 整字转发（DSP 存储器按 32 位字寻址），凑不齐的 0~3 字节留在暂存区等下一块。
  */
static int usbi_ep0_out_stream(struct usb_setup_packet *setup, uint8_t *data,
                               uint32_t len, uint32_t offset, uint32_t total)
{
    uint32_t skip;
    uint32_t n;
    uint32_t room;
    uint32_t i;

    if (setup->bRequest != USBI_REQ_WRITE_REGISTER) {
        return -1;                      /* 只有写寄存器请求支持超长传输 */
    }
    if ((total < 3u) || !s_bus_ready) {
        return -1;
    }

    if (offset == 0u) {
        if (len < 2u) {
            return -1;
        }
        s_stream_reg = (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
        s_stream_data_written = 0u;
        s_stream_stage_len = 0u;
        s_write_status = USBI_ST_SUCCESS;   /* 新请求：状态从头开始记 */
    }

    /* 请求头那 2 个字节只在第一块里，且不是数据 */
    skip = (offset < 2u) ? (2u - offset) : 0u;
    if (skip >= len) {
        return 0;
    }

    /* ① 把本块的数据部分追加到暂存区。
     *    每次转发后暂存区里最多剩 3 字节，所以 room 恒 >= 256+1，够放一整块 */
    room = (USBI_EP0_STREAM_CHUNK + 4u) - (uint32_t)s_stream_stage_len;
    if ((len - skip) > room) {
        return -1;
    }
    for (i = skip; i < len; i++) {
        s_stream_stage[s_stream_stage_len++] = data[i];
    }

    /* ② 整字（4 字节）转发，只要凑够就发；最后一块把尾巴也发掉 */
    if ((offset + len) >= total) {
        n = s_stream_stage_len;
    } else {
        n = (uint32_t)s_stream_stage_len & ~3u;
    }

    if (n > 0u) {
        if (bus_write_at(s_stream_reg, (uint16_t)s_stream_data_written,
                         s_stream_stage, (uint16_t)n) != 0) {
            s_write_status = USBI_ST_FAILED;   /* 总线失败要传给 0xB5 */
            return -1;
        }
        s_stream_data_written += n;
        memmove(s_stream_stage, &s_stream_stage[n], (uint32_t)s_stream_stage_len - n);
        s_stream_stage_len = (uint8_t)(s_stream_stage_len - (uint8_t)n);
    }

    if ((offset + len) >= total) {
        /* 整条请求处理完了：与普通写请求一样统计一次 */
        usbi_led_notify_write();
        flash_track(s_stream_reg,
                    (s_stream_data_written > 0xFFFFu) ? 0xFFFFu : (uint16_t)s_stream_data_written);
    }
    return 0;
}

/* ========================= vendor 请求处理 ========================= */
/**
  * @brief  EP0 vendor 请求处理（CherryUSB 回调）
  * @param  setup 8 字节 SETUP 包
  * @param  data  数据指针；OUT 方向指向已收齐的 req_data，IN 方向由本函数赋值
  * @param  len   数据长度
  * @retval 0 = 已处理；非 0 = 交给 core 继续找（最终会 STALL）
  */
static int usbi_vendor_request(struct usb_setup_packet *setup, uint8_t **data, uint32_t *len)
{
    uint32_t wlen = setup->wLength;

    /* ---------------- 主机 → 设备（OUT） ---------------- */
    if ((setup->bmRequestType & USB_REQUEST_DIR_MASK) == USB_REQUEST_DIR_OUT) {
        uint8_t *payload = *data;

        switch (setup->bRequest) {
        case USBI_REQ_WRITE_REGISTER: { /* 0xB2：写寄存器，payload = 寄存器号 + 数据 */
            uint16_t reg;

            if ((wlen < 2u) || (wlen > USBI_EP0_MAX_XFER) || (payload == NULL)) {
                return -1;
            }
            if (!s_bus_ready) {
                return -1;
            }

            reg = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);

#if USBI_RESET_MAGIC_REG_ENABLE
            /* 暂定的复位控制方式：写"寄存器" 0xFFFF，数据第 1 字节非零 = 复位 */
            if (reg == USBI_RESET_MAGIC_REG) {
                if (wlen >= 3u) {
                    usbi_target_reset(payload[2] ? 1 : 0);
                }
                *len = 0;
                return 0;
            }
#endif

            s_write_status = (bus_write(reg, &payload[2], (uint16_t)(wlen - 2u)) == 0)
                             ? USBI_ST_SUCCESS : USBI_ST_FAILED;

            usbi_led_notify_write();
            flash_track(reg, (uint16_t)(wlen - 2u));

            *len = 0;
            return 0;   /* 状态阶段由 CherryUSB 自动发送 */
        }

        case USBI_REQ_READ_REGISTER: { /* 0xB3：登记要读的寄存器，等 0xB4 再真正取数 */
            if ((wlen < 2u) || (payload == NULL)) {
                return -1;
            }
            s_pending_read_reg = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
            s_pending_read_valid = 1u;
            *len = 0;
            return 0;
        }

        default:
            return -1;  /* 未知请求 → core 会 STALL */
        }
    }

    /* ---------------- 设备 → 主机（IN） ---------------- */
    switch (setup->bRequest) {
    case USBI_REQ_READ_REGISTER_RESPONSE: { /* 0xB4：返回刚登记的寄存器的内容 */
        uint32_t rd_len = wlen;

        if (!s_pending_read_valid) {
            return -1;
        }
        if (rd_len > USBI_READ_BUF_SIZE) {
            rd_len = USBI_READ_BUF_SIZE;
        }
        if (rd_len) {
            if (!s_bus_ready) {
                return -1;
            }
            if (bus_read(s_pending_read_reg, s_read_buf, (uint16_t)rd_len) != 0) {
                /* 总线出错：补零，但绝不能破坏控制传输的包长/对齐 */
                memset(s_read_buf, 0, rd_len);
                s_read_status = USBI_ST_FAILED;
            } else {
                s_read_status = USBI_ST_SUCCESS;
            }
        }

        s_pending_read_valid = 0u;
        usbi_led_notify_read();

        *data = s_read_buf;
        *len = rd_len;
        return 0;
    }

    case USBI_REQ_READ_REGISTER_ACK:          /* 0xB5：上一次写是否成功 */
        *data = &s_write_status;
        *len = 1u;
        return 0;
    case USBI_REQ_READ_REGISTER_RESPONSE_ACK: /* 0xB6：上一次读是否成功 */
        *data = &s_read_status;
        *len = 1u;
        return 0;

    default:
        return -1;
    }
}

/**
  * @brief USB 事件通知：把"已配置/复位"映射到指示灯状态
  */
static void usbi_notify_handler(uint8_t event, void *arg)
{
    (void)arg;

    switch (event) {
    case USBD_EVENT_CONFIGURED:
        usbi_led_set_connected(1u);
        break;
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        usbi_led_set_connected(0u);
        break;
    default:
        break;
    }
}

/* ========================= 对外接口 ========================= */
void usbi_usb_init(void)
{
    usbd_desc_register(usbi_descriptor);

    memset(&usbi_intf, 0, sizeof(usbi_intf));
    usbi_intf.vendor_handler = usbi_vendor_request;
    usbi_intf.notify_handler = usbi_notify_handler;

    usbd_add_interface(&usbi_intf);   /* intf_num 由 core 自动填写 */
    /* 本设备没有非 0 端点，因此不需要 usbd_add_endpoint() */

    /* 注册超长块写的流式接收（烧 EEPROM 时会有 12 KB 级的单次写请求） */
    usbd_ep0_out_stream_config(s_stream_buf, USBI_EP0_STREAM_CHUNK, usbi_ep0_out_stream);

    usbd_initialize();
}

void usbi_init(void)
{
    /* USBI_RESET_ENABLE = 1（V1.0）：把 PB0 驱动成确定的高电平，释放并保持
     * 目标的复位（gpio.c 里 PB0 也是推挽输出，这里只是再确保一次电平）。
     * USBI_RESET_ENABLE = 0 时改为把 PB0 置成高阻，固件完全不碰复位线。 */
    reset_pin_idle();

#if USBI_RESET_BOOT_PULSE
    /* 可选：上电复位一次 */
    usbi_target_reset_pulse();
#endif

    /* 片选引脚先准备好并保持高电平（= SS/ADDR0 高：I2C 地址 0x3B，
     * 而且不会给目标那 3 个"切到 SPI"的脉冲） */
    spi_io_init();

    /* 按 USBI_BUS_SELECT 选总线：
     *   USBI_BUS_SPI  —— 默认，保持原有行为
     *   USBI_BUS_I2C  —— 硬件 I2C1（默认 PA9=SCL / PA10=SDA，400 kHz）
     *   USBI_BUS_AUTO —— 先探测 I2C 从机，探测不到退回 SPI
     * 三条路都会自己调用 spi_io_init()/i2c_bus_setup() 并把指示灯总线色设好。 */
#if (USBI_BUS_SELECT == USBI_BUS_I2C)
    usbi_bus_set(USBI_BUS_I2C);
#elif (USBI_BUS_SELECT == USBI_BUS_AUTO)
    usbi_bus_set(USBI_BUS_AUTO);
#else
    usbi_bus_set(USBI_BUS_SPI);
#endif

    s_bus_ready = 1u;
    usbi_led_set_bus(s_bus);   /* 让指示灯用颜色区分 SPI / I2C */
}

/**
  * @brief 主循环任务
  *
  * 只做两件事：
  *   ① 烧录活动停了 USBI_BURN_IDLE_MS 就收掉烧录灯（烧录中途被取消 / 擦除等待太长）；
  *   ② 消费"主机已启动 DSP"标记 —— 默认什么都不做（**绝不复位目标**）。
  *
  * 这里不能加"下载结束后自动复位一次目标"：抓包实测最后一次大块写（`DM1 Data`）
  * 与 SigmaStudio 启动 DSP 的 START_ADDRESS/KILL_CORE/START_CORE/START_PULSE 只差
  * 几毫秒，任何基于"超时"的复位都会把刚启动的程序冲掉 → DSP 不出声。
  */
void usbi_task(void)
{
    if (s_burn_active && ((uint32_t)(HAL_GetTick() - s_burn_last_ms) > USBI_BURN_IDLE_MS)) {
        s_burn_active = 0u;
        usbi_led_set_flashing(0u);
    }

    if (s_session_started) {
        s_session_started = 0u;
#if USBI_RESET_ON_SESSION_END
        /* 可选：主机刚启动 DSP 时补一个复位脉冲（用于验证 EEPROM 自举）。
         * 默认关闭 —— 打开后目标会立刻从自己的 EEPROM 重新自举，
         * 如果 EEPROM 里没有有效镜像，声音会直接停掉。 */
        usbi_target_reset_pulse();
#endif
    }
}

/**
  * @brief  覆盖 CherryUSB 端口层的空实现，补上 PY32 的 USB 时钟使能
  */
void usb_dc_low_level_init(void)
{
    __HAL_RCC_USB_CLK_ENABLE();
}

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
