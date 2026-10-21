# unused：软件模拟 I2C 主机（PY32 USBi 备用实现）

## 这是什么

`Core/SRC/usbi.c` 里原本有一份**纯软件模拟的 I2C 主机**（GPIO 开漏 + `SysTick->VAL`
定时），在 2026-10-20 那一版里曾经是 `USBI_BUS_I2C` 的实际实现，离线用 Python
位级仿真验过 6 个用例（字节序/位序、ACK 采样、重复 START、248 字节大块、
错地址 NACK、字节偏移→字地址换算）全部通过。

现在工程改用**硬件 I2C1 + HAL**（正版 USBi 的 DSP 侧 I2C 固定 400 kHz，外设能
跑满标称速率，抖动也比软件模拟小），这一段就整段退休了。

## 为什么留着

- 如果哪天要换引脚（例如真 USBi 那种 I2C 与被控 SPI 同一条线的 10 针口，
  或者板子把 SDA/SCL 接到了别的脚），这段代码只依赖 GPIO，改两个宏就能用。
- 如果哪天 USB 中断里不允许再阻塞（例如想把总线事务挪到主循环），
  软件模拟的"每一处等待都有硬上界"比 HAL 更适合放到协作式调度里。
- 它是一个能直接对着 I2C 总线做比特级试探的调试工具：`i2c_probe_addr()` /
  `i2c_bus_unstick()` 可以单独抠出来排查"从机把 SDA 拉住"这类问题。

## 怎么恢复

1. 把下面 `c` 代码块的内容放回 `Core/SRC/usbi.c`，位置在 `bus_spi_read()` 之后、
   `总线分发` 注释之前（就是它原来待的地方）。
2. 删掉现在的硬件 I2C 段（`I2C 后端（硬件 I2C1 + HAL）` 那一整块）。
3. `Core/INC/usbi.h` 里把 `USBI_I2C_CLOCK_HZ` 改回 `100000u`（软件模拟在
   100 kHz 最稳，400 kHz 需要 CPU 硬算半周期，抖动大）。
4. `Core/INC/usbi.h` 里补回 `USBI_I2C_STRETCH_GUARD 20000u`（软件模拟用它
   限制时钟拉伸的等待次数），硬件版不需要。
5. 编译期宏 `USBI_BUS_SELECT` 不用动，`USBI_BUS_SPI / I2C / AUTO` 的语义完全一样。

> 注意：软件模拟版**不依赖** SysTick 的优先级（它用 `SysTick->VAL` 硬件计数），
> 所以恢复它之后，`TICK_INT_PRIORITY = 0` 这个改动就不是必需的了 —— 但留着也
> 没有坏处（它只让 `HAL_Delay`/HAL 超时在中断里也能用）。

## 代码（原样归档）

```c
/* ========================= I2C 后端（软件模拟主机） =========================
 * 为什么不直接用硬件 I2C1 + HAL：
 *   所有 vendor 请求都在 USB 中断里执行，而 SysTick 的优先级是最低的
 *   （py32f071_hal_conf.h 的 TICK_INT_PRIORITY = PRIORITY_LOWEST），USB 中断
 *   期间它进不来 → HAL_GetTick() 不前进 → HAL_I2C 里"超时就报错"的判断永远
 *   不成立。总线一旦被拉死（SDA 被别人拉住、插拔瞬间的半个字节），中断就永久
 *   卡在 `while (!flag)` 里，只能等 IWDG 把整机复位、USB 重枚举。
 *   软件模拟的每一处等待都有硬上界（轮询计数 / SysTick 计数），最坏情况只是
 *   "这一次事务返回失败"，中断照样退出，下一次请求还能重来。
 *
 * 时序不追求标称值：半周期用 SysTick 计数器做延迟，实际频率比标称低几个百分点
 * （I2C 没有下限），对 ADAU 完全够用；也不占用 I2C1 外设和它的 DMA。
 *
 * 硬件约束（ADAU1452/1451/1450 Rev.D）：
 *   - 从机口上电默认 I2C；SS/ADDR0 被拉低 3 次就永久进入 SPI 模式，回 I2C 只能
 *     靠 RESET 引脚硬件复位或断电（软复位 0xF890 也不行）。
 *   - SCL/SDA 各需一个 2 kΩ 上拉（只有 MCU 内部上拉时请把时钟降到 20 kHz 以下）。
 *   - 地址低 2 位由 SS/ADDR0、MOSI/ADDR1 硬接线决定（都悬空 = 0x3B）。
 *   - 一次事务 = START + 从机地址 + 2 字节大端子地址 + N 字节 + STOP；
 *     读事务在 2 字节子地址之后用**重复 START**（不发 STOP）再读。
 * ========================================================================= */

/* 目标 7 位地址（i2c_bus_setup() 探测得到，探测不到时用默认的 0x3B） */
static uint8_t s_i2c_addr = USBI_I2C_ADDR_DEFAULT;

/* SCL 半周期的 SysTick 计数（i2c_bus_setup() 里按 USBI_I2C_CLOCK_HZ 算一次） */
static uint32_t s_i2c_half_ticks;

/* 半周期的下限：留够 GPIO 翻转本身的开销 */
#define I2C_HALF_TICKS_MIN 6u

/* 引脚直连 BSRR/IDR：走 HAL_GPIO_WritePin 每次要多几十个周期，
 * 400 kHz（半周期 60 个周期）根本不够用 */
#define I2C_SCL_PORT  USBI_I2C_SCL_PORT
#define I2C_SCL_PIN   USBI_I2C_SCL_PIN
#define I2C_SDA_PORT  USBI_I2C_SDA_PORT
#define I2C_SDA_PIN   USBI_I2C_SDA_PIN

#define I2C_SCL_HIGH()  (I2C_SCL_PORT->BSRR = (uint32_t)I2C_SCL_PIN)
#define I2C_SCL_LOW()   (I2C_SCL_PORT->BSRR = ((uint32_t)I2C_SCL_PIN << 16u))
#define I2C_SDA_HIGH()  (I2C_SDA_PORT->BSRR = (uint32_t)I2C_SDA_PIN)
#define I2C_SDA_LOW()   (I2C_SDA_PORT->BSRR = ((uint32_t)I2C_SDA_PIN << 16u))
#define I2C_SDA_LEVEL() (((I2C_SDA_PORT->IDR & I2C_SDA_PIN) != 0u) ? 1u : 0u)
#define I2C_SCL_LEVEL() (((I2C_SCL_PORT->IDR & I2C_SCL_PIN) != 0u) ? 1u : 0u)

/* 半个 SCL 周期 */
#define I2C_HALF() i2c_wait_ticks(s_i2c_half_ticks)

/**
  * @brief 忙等 ticks 个 SysTick 计数
  *
  * 只读 SysTick 的硬件计数器，不用 HAL_GetTick()：本段代码在 USB 中断里跑，
  * SysTick 中断进不来，uwTick 是冻住的，但计数器本身一直在走。
  */
static void i2c_wait_ticks(uint32_t ticks)
{
    uint32_t start;
    uint32_t load;
    uint32_t now;
    uint32_t elapsed;

    if (ticks == 0u) {
        return;
    }
    start = SysTick->VAL;
    load = SysTick->LOAD + 1u;
    if (ticks >= load) {
        ticks = load - 1u;          /* 一次最多等一个 SysTick 周期 */
    }
    do {
        now = SysTick->VAL;
        elapsed = (start >= now) ? (start - now) : (start + load - now);
    } while (elapsed < ticks);
}

/**
  * @brief 微秒级等待（同样只用 SysTick 计数器）
  */
static void i2c_delay_us(uint32_t us)
{
    if (us == 0u) {
        return;
    }
    i2c_wait_ticks(us * (SystemCoreClock / 1000000u));
}

/**
  * @brief 把 SCL 释放成高，并等它真的变高（从机可能在做时钟拉伸）
  * @retval 0 = 已变高；-1 = 等超时（从机把 SCL 永久拉死）
  */
static int i2c_scl_release(void)
{
    uint32_t guard = USBI_I2C_STRETCH_GUARD;

    I2C_SCL_HIGH();
    while (I2C_SCL_LEVEL() == 0u) {
        if (--guard == 0u) {
            return -1;
        }
    }
    return 0;
}

/**
  * @brief 引脚初始化：开漏输出 + 内部上拉（外部 2 kΩ 才是主力）
  */
static void i2c_io_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 硬件 I2C1 让出引脚（Studio_I2C1_Init 已经把它配成 AF 开漏），
     * 之后由本文件的模拟主机自己控线 */
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
  * @brief 总线恢复：从机把 SDA 拉住不放时，补最多 9 个时钟把它"顶"出来
  *
  * 只在 START 前/事务失败后调用。9 个时钟足以让任何卡在半个字节中间的从机
  * 把剩下的数据位吐完、放开 SDA。全程有上界，绝不会死等。
  *
  * @retval 0 = 总线已空闲；-1 = SCL 被永久拉死（本事务放弃）
  */
static int i2c_bus_unstick(void)
{
    uint8_t i;

    I2C_SDA_HIGH();
    if (i2c_scl_release() != 0) {
        return -1;
    }
    i2c_delay_us(1);

    if (I2C_SDA_LEVEL() != 0u) {
        return 0;                   /* 两条线都高 = 总线本来就空闲 */
    }

    for (i = 0u; i < 9u; i++) {
        I2C_SCL_LOW();
        I2C_HALF();
        if (i2c_scl_release() != 0) {
            return -1;
        }
        I2C_HALF();
        if (I2C_SDA_LEVEL() != 0u) {
            break;                  /* 从机放开了 */
        }
    }

    /* 补一个 STOP，把总线拉回空闲态 */
    I2C_SCL_LOW();
    I2C_SDA_LOW();
    I2C_HALF();
    if (i2c_scl_release() != 0) {
        return -1;
    }
    I2C_HALF();
    I2C_SDA_HIGH();
    I2C_HALF();
    return 0;
}

/**
  * @brief 发 STOP：SCL 低时拉低 SDA → 抬起 SCL → 抬起 SDA
  */
static void i2c_stop(void)
{
    I2C_SCL_LOW();
    I2C_SDA_LOW();
    I2C_HALF();
    I2C_SCL_HIGH();
    I2C_HALF();
    I2C_SDA_HIGH();
    I2C_HALF();
}

/**
  * @brief 发 START（总线忙时自动就是重复 START）
  * @retval 0 = 成功；-1 = 总线被拉死，或 SDA 已被别人拉着（此时 START 会变成 STOP）
  */
static int i2c_start(void)
{
    I2C_SDA_HIGH();
    if (i2c_scl_release() != 0) {
        return -1;
    }
    if (I2C_SDA_LEVEL() == 0u) {
        return -1;
    }
    I2C_HALF();
    I2C_SDA_LOW();                  /* SCL 高时 SDA 下降 = START */
    I2C_HALF();
    I2C_SCL_LOW();
    I2C_HALF();
    return 0;
}

/**
  * @brief 发一个字节（MSB first）并读回第 9 个时钟的 ACK
  * @retval 0 = 从机 ACK；-1 = NACK 或 SCL 拉伸超时
  */
static int i2c_write_byte(uint8_t value)
{
    uint8_t i;
    uint8_t nack;

    for (i = 0u; i < 8u; i++) {
        if ((value & 0x80u) != 0u) {
            I2C_SDA_HIGH();
        } else {
            I2C_SDA_LOW();
        }
        value = (uint8_t)(value << 1u);
        I2C_HALF();
        if (i2c_scl_release() != 0) {
            return -1;
        }
        I2C_HALF();
        I2C_SCL_LOW();
        I2C_HALF();
    }

    /* 第 9 个时钟：主机释放 SDA，从机拉低表示 ACK */
    I2C_SDA_HIGH();
    I2C_HALF();
    if (i2c_scl_release() != 0) {
        return -1;
    }
    I2C_HALF();
    nack = I2C_SDA_LEVEL();         /* 1 = NACK */
    I2C_SCL_LOW();
    I2C_HALF();
    return (nack != 0u) ? -1 : 0;
}

/**
  * @brief 读一个字节，并在第 9 个时钟回 ACK / NACK
  * @param ack 1 = 回 ACK（后面还有数据），0 = 回 NACK（这是最后一字节）
  * @retval 0 = 成功；-1 = SCL 拉伸超时
  */
static int i2c_read_byte(uint8_t *out, uint8_t ack)
{
    uint8_t i;
    uint8_t value = 0u;

    I2C_SDA_HIGH();                 /* 主机释放 SDA，交给从机驱动 */
    for (i = 0u; i < 8u; i++) {
        I2C_HALF();
        if (i2c_scl_release() != 0) {
            return -1;
        }
        value = (uint8_t)((value << 1u) | (uint8_t)I2C_SDA_LEVEL());
        I2C_HALF();
        I2C_SCL_LOW();
    }
    I2C_HALF();

    if (ack != 0u) {
        I2C_SDA_LOW();
    } else {
        I2C_SDA_HIGH();
    }
    I2C_HALF();
    if (i2c_scl_release() != 0) {
        return -1;
    }
    I2C_HALF();
    I2C_SCL_LOW();
    I2C_HALF();
    I2C_SDA_HIGH();

    *out = value;
    return 0;
}

/**
  * @brief 一次写事务：START + 地址(W) + 2 字节子地址 + 数据 + STOP
  */
static int i2c_xfer_write(uint16_t sub, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    int rc = -1;

    if (i2c_start() != 0) {
        i2c_stop();
        return -1;
    }
    if (i2c_write_byte((uint8_t)(s_i2c_addr << 1)) != 0) {
        goto out;
    }
    if (i2c_write_byte((uint8_t)(sub >> 8)) != 0) {
        goto out;
    }
    if (i2c_write_byte((uint8_t)(sub & 0xFFu)) != 0) {
        goto out;
    }
    for (i = 0u; i < len; i++) {
        if (i2c_write_byte(data[i]) != 0) {
            goto out;
        }
    }
    rc = 0;
out:
    i2c_stop();
    return rc;
}

/**
  * @brief 一次读事务：START + 地址(W) + 2 字节子地址 + 重复 START + 地址(R) + 数据 + STOP
  */
static int i2c_xfer_read(uint16_t sub, uint8_t *data, uint16_t len)
{
    uint16_t i;
    int rc = -1;

    if (i2c_start() != 0) {
        i2c_stop();
        return -1;
    }
    if (i2c_write_byte((uint8_t)(s_i2c_addr << 1)) != 0) {
        goto out;
    }
    if (i2c_write_byte((uint8_t)(sub >> 8)) != 0) {
        goto out;
    }
    if (i2c_write_byte((uint8_t)(sub & 0xFFu)) != 0) {
        goto out;
    }
    if (i2c_start() != 0) {         /* 重复 START：不发 STOP */
        goto out;
    }
    if (i2c_write_byte((uint8_t)((s_i2c_addr << 1) | 1u)) != 0) {
        goto out;
    }
    for (i = 0u; i < len; i++) {
        uint8_t more = (uint8_t)(((i + 1u) < len) ? 1u : 0u);
        if (i2c_read_byte(&data[i], more) != 0) {
            goto out;
        }
    }
    rc = 0;
out:
    i2c_stop();
    return rc;
}

/**
  * @brief 单字节"探活"事务：START + 地址(W) + STOP，看从机 ACK 不 ACK
  */
static int i2c_probe_addr(uint8_t addr7)
{
    int rc;

    if (i2c_start() != 0) {
        i2c_stop();
        return -1;
    }
    rc = i2c_write_byte((uint8_t)(addr7 << 1));
    i2c_stop();
    return rc;
}

/**
  * @brief 带重试的事务（每次重试前先做总线恢复）
  */
static int i2c_txn(uint8_t write, uint16_t sub, uint8_t *data, uint16_t len)
{
    uint8_t tries = USBI_I2C_RETRIES;

    do {
        int rc;

        if (i2c_bus_unstick() != 0) {
            return -1;              /* SCL 被拉死，重试也没意义 */
        }
        rc = (write != 0u) ? i2c_xfer_write(sub, data, len)
                           : i2c_xfer_read(sub, data, len);
        if (rc == 0) {
            return 0;
        }
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
            if (i2c_probe_addr(addr) == 0) {
                *addr_out = addr;
                return 0;
            }
            (void)i2c_bus_unstick();
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
  * @brief I2C 写：按区域分块，每块 = 2 字节子地址 + 数据
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
  * @brief I2C 读：写子地址 + 重复 START + 读数据
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
  * @brief 准备 I2C 主机并探测目标
  *
  * 只在主循环上下文调用（usbi_init / usbi_bus_set）。
  *
  * @retval 0 = 探测到目标；-1 = 总线上没有响应（主机照样准备好，后续传输会失败）
  */
static int i2c_bus_setup(void)
{
    uint32_t half;

    i2c_io_init();

    half = (uint32_t)(SystemCoreClock / USBI_I2C_CLOCK_HZ) / 2u;
    if (half < I2C_HALF_TICKS_MIN) {
        half = I2C_HALF_TICKS_MIN;
    }
    s_i2c_half_ticks = half;

    /* 先把可能卡住的总线放开，再探测 */
    (void)i2c_bus_unstick();

    s_i2c_addr = USBI_I2C_ADDR_DEFAULT;
    return i2c_probe(&s_i2c_addr);
}

```

## 附：这一段的接口（与硬件版一致，替换时上层不用改）

| 函数 | 作用 |
|---|---|
| `i2c_wait_ticks()` / `i2c_delay_us()` | 基于 `SysTick->VAL` 的有界延时 |
| `i2c_scl_release()` | 释放 SCL 并等它真的变高（带拉伸上限） |
| `i2c_io_init()` | 把 I2C1 的引脚夺过来当开漏 GPIO |
| `i2c_bus_unstick()` | 9 个时钟 + STOP，把卡住的从机顶出来 |
| `i2c_start/stop/write_byte/read_byte` | 位级时序（数据在 SCL 低时改、高时采样，第 9 时钟读 ACK） |
| `i2c_xfer_write/read` | 一次事务：子地址 2 字节 + 数据；读用重复 START |
| `i2c_probe_addr/i2c_probe` | 0x38~0x3B 逐地址探测 |
| `i2c_next_sub/i2c_chunk` | 子地址自增规则与分块上限（与硬件版相同） |
| `bus_i2c_write/read` | 按区域分块 + 程序区 300 µs settle |
| `i2c_bus_setup()` | 配引脚、算半周期、探测目标 |
