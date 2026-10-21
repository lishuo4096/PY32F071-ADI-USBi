/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbi_led.c
  * @brief   USBi 状态指示灯状态机（WS2812 + TIM14 20 ms 节拍）
  *
  *          两个关键机制：
  *          ① "色相 = 总线模式"：SPI 绿色、I2C 紫色，任何时刻都能看出走哪条总线；
  *          ② "一条命令 = 一个完整脉冲"：USB 中断里的 notify_write/read() 只登记
  *             脉冲参数并置 pending，主循环 usbi_led_task() 一看到 pending 就
  *             立刻渲染点亮帧（微秒级响应，不等 20 ms 节拍），随后由节拍把
  *             亮/暗相位推进完，再回到呼吸。脉冲进行中不重新触发。
  *
  *          呼吸/闪灯的相位推进只发生在 20 ms 节拍上，主循环负担极小。
  *
  *          显示档位由 usbi_led.h 的 USBI_LED_MODE 在编译期选定（1/2/3）：
  *             1 = 未枚举白呼吸 / 枚举后模式色 / 烧录闪      （老行为）
  *             2 = 未枚举灭   / 枚举后模式色 / 烧录闪
  *             3 = 常灭       / 只有烧录时闪
  *          档位的差别只在"亮度怎么被压成 0"，色相/节拍逻辑三档共用。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbi_led.h"
#include "usbi.h"
#include "ws2812.h"
#include "tim.h"

/* Private variables ---------------------------------------------------------*/
static volatile uint8_t s_tick;              /* TIM14 每 20 ms 置 1 */
static ws2812_crgb_t    s_color;             /* 当前底色 */
static uint8_t          s_level = 255u;      /* 当前亮度 0~255 */
static uint8_t          s_breathe;           /* 呼吸相位 */
static uint8_t          s_phase;             /* 烧录闪灯相位 */

static volatile uint8_t s_state = USBI_LED_DISCONNECTED;
static volatile uint8_t s_connected;
static volatile uint8_t s_flashing;
static volatile uint8_t s_bus = USBI_BUS_SPI;/* 当前总线模式：只影响色相 */

/* 一次性脉冲（一条命令 = 一次亮灭） */
static volatile uint8_t s_pulse_ticks;       /* 剩余节拍数，0 = 无脉冲 */
static volatile uint8_t s_pulse_off;         /* 暗相位持续多少个节拍 */
static volatile uint8_t s_pulse_level;       /* 亮相位亮度 */
static volatile uint8_t s_pulse_pending;     /* 需要在主循环里立刻渲染一帧 */

/* 烧录闪灯期间的"读命令凹口"：为 1 时这一帧强制压暗，节拍里清掉
 * （写命令走 flash_accent 拉亮，读命令走 flash_read_accent 压暗，
 *   两者在"灯一直亮着"的烧录过程中才能各自看得见） */
static volatile uint8_t s_flash_dip;

/* 档位 3：枚举成功后还要呼吸多少拍（0 = 招呼打完了，之后只有活动才亮） */
static volatile uint16_t s_announce_ticks;

/* 上一帧真正发上灯线的颜色：和这一帧完全一样就不重发
 * （模式 3 下整颗灯平时不亮，靠这个把 20 ms 一次的无效帧彻底省掉） */
static uint8_t s_shown_valid;
static uint8_t s_shown_r;
static uint8_t s_shown_g;
static uint8_t s_shown_b;

/* Private function prototypes -----------------------------------------------*/
static uint8_t scale8(uint8_t component, uint8_t level);
static void    mode_color(uint8_t *r, uint8_t *g, uint8_t *b);
static uint8_t square(uint8_t phase, uint8_t half);
static uint8_t breathe_level(void);
static void    led_render(void);
static void    render_state(void);
static void    start_pulse(uint8_t state, uint8_t on, uint8_t off, uint8_t level);
static void    flash_accent(void);
static void    flash_read_accent(void);

/* Private functions ---------------------------------------------------------*/
static uint8_t scale8(uint8_t component, uint8_t level)
{
    return (uint8_t)(((uint16_t)component * (uint16_t)level) / 255u);
}

/**
  * @brief  当前总线模式对应的色相：SPI 绿、I2C 紫
  */
static void mode_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s_bus == USBI_BUS_I2C) {
        *r = USBI_LED_I2C_R; *g = USBI_LED_I2C_G; *b = USBI_LED_I2C_B;
    } else {
        *r = USBI_LED_SPI_R; *g = USBI_LED_SPI_G; *b = USBI_LED_SPI_B;
    }
}

/**
  * @brief  方波：每 half*2 拍一个周期，返回 1 表示"亮"相位
  */
static uint8_t square(uint8_t phase, uint8_t half)
{
    if (half == 0u) {
        return 1u;
    }
    return (uint8_t)((phase / half) & 1u);
}

/**
  * @brief  由呼吸相位算出 0~255 的亮度（三角波 + 平方律柔和化）
  *
  * 全程用 uint16_t 中间量并显式夹紧：level ≤ 255 时 level*level ≤ 65025，
  * 不会回绕；即便把 BREATHE_STEP 改大导致相位跳过极值点，也只是波形变陡，
  * 不会串到别的颜色/状态上去。
  *
  * 算完再按 USBI_LED_PEAK_LEVEL 压一次峰值（默认 255 即不压）。
  */
static uint8_t breathe_level(void)
{
    uint16_t level;
    uint16_t phase = s_breathe;

    if (phase < 128u) {
        level = (uint16_t)(phase * 2u);
    } else {
        level = (uint16_t)((255u - phase) * 2u);
    }

    level = (uint16_t)((level * level) / 255u);

    if (level > 255u) {
        level = 255u;
    }

#if (USBI_LED_PEAK_LEVEL < 255u)
    level = (uint16_t)((level * (uint16_t)USBI_LED_PEAK_LEVEL) / 255u);
#endif

    return (uint8_t)level;
}

/**
  * @brief  把 s_color/s_level 写到灯珠（非阻塞）
  *
  * 和上一帧颜色完全一样就直接返回：WS2812 是"保持型"器件，不重发等于不变。
  * 注意只有 show() 真的把帧发出去了才更新缓存 —— 丢帧（上一帧还在发）时
  * 不缓存，下一拍会再试一次。
  */
static void led_render(void)
{
    uint8_t r = scale8(s_color.r, s_level);
    uint8_t g = scale8(s_color.g, s_level);
    uint8_t b = scale8(s_color.b, s_level);

    if (s_shown_valid && (r == s_shown_r) && (g == s_shown_g) && (b == s_shown_b)) {
        return;
    }

    ws2812_set_all(r, g, b);
    if (ws2812_show()) {
        s_shown_r = r;
        s_shown_g = g;
        s_shown_b = b;
        s_shown_valid = 1u;
    }
}

/**
  * @brief  按 s_state 决定这一帧的颜色和亮度
  */
static void render_state(void)
{
    switch (s_state) {
    case USBI_LED_CONNECTED:                 /* 模式色呼吸 */
        mode_color(&s_color.r, &s_color.g, &s_color.b);
        s_level = breathe_level();
        break;

    case USBI_LED_WRITING:                   /* 写入脉冲：固定蓝色，亮 ON 拍 → 暗 OFF 拍 */
        s_color.r = USBI_LED_WRITE_R;
        s_color.g = USBI_LED_WRITE_G;
        s_color.b = USBI_LED_WRITE_B;
        s_level = (s_pulse_ticks > s_pulse_off) ? s_pulse_level : USBI_LED_DARK_LEVEL;
        break;

    case USBI_LED_READING:                   /* 读出脉冲：固定青色，更暗、暗相位更长 */
        s_color.r = USBI_LED_READ_R;
        s_color.g = USBI_LED_READ_G;
        s_color.b = USBI_LED_READ_B;
        s_level = (s_pulse_ticks > s_pulse_off) ? s_pulse_level : USBI_LED_DARK_LEVEL;
        break;

    case USBI_LED_FLASHING:                  /* 紫色高频闪 */
        s_color.r = USBI_LED_BURN_R;
        s_color.g = USBI_LED_BURN_G;
        s_color.b = USBI_LED_BURN_B;
        /* 读命令刚到的这一帧强制压暗（s_flash_dip），做出一个"凹口" */
        s_level = (square(s_phase, USBI_LED_FLASH_HALF) && (s_flash_dip == 0u))
                  ? 255u : 16u;
        break;

    case USBI_LED_DISCONNECTED:              /* 白色呼吸 */
    default:
        s_color.r = USBI_LED_OFF_R;
        s_color.g = USBI_LED_OFF_G;
        s_color.b = USBI_LED_OFF_B;
        s_level = breathe_level();
        break;
    }

    /* ---- 按编译期档位把不该亮的场景压成 0 ---- */
#if (USBI_LED_MODE == USBI_LED_MODE_AFTER_ENUM)
    /* 档位 2：没枚举就不亮。枚举之后呼吸/读/写/烧录全部照旧。 */
    if (!s_connected) {
        s_level = 0u;
    }
#elif (USBI_LED_MODE == USBI_LED_MODE_ACTIVITY_ONLY)
    /* 档位 3：平时全灭，只有"总线上有活动"才亮。
     * 读脉冲、写脉冲、烧录闪都算活动；"空闲呼吸"只保留枚举成功后的一小段
     * 招呼时间（s_announce_ticks 用完就彻底灭），用来确认"认到主机了"。 */
    if ((s_state == USBI_LED_CONNECTED) || (s_state == USBI_LED_DISCONNECTED)) {
        if (s_announce_ticks == 0u) {
            s_level = 0u;
        }
    }
#endif

    led_render();
}

/**
  * @brief  启动一次"亮 → 暗"脉冲；已在脉冲中则不重触发
  *
  * 只登记参数并置 pending，真正的渲染交给主循环（不在 USB 中断里碰 DMA）。
  */
static void start_pulse(uint8_t state, uint8_t on, uint8_t off, uint8_t level)
{
    if (s_pulse_ticks != 0u) {
        return;                     /* 上一个脉冲还没走完 */
    }
    if ((on == 0u) && (off == 0u)) {
        return;
    }

    s_pulse_off = off;
    s_pulse_level = level;
    s_pulse_ticks = (uint8_t)(on + off);
    s_state = state;
    s_pulse_pending = 1u;
}

/**
  * @brief  烧录闪灯期间的"命令响应"：把闪灯相位立刻拉回亮相位并渲染一帧
  *
  * 为什么不是另起一个脉冲：烧录状态优先级最高，蓝色/青色脉冲会被
  * set_flashing() 直接吞掉，于是在烧录过程中写 2 字节 / 4 字节寄存器
  * 完全没有反馈。这里改成"命令把闪灯踢回亮"：
  *
  *   - 命令落在暗相位上 → 灯立刻变亮，一条命令一次可见的响应；
  *   - 命令密集时灯基本保持亮（读起来就是"正在拼命烧"）；
  *   - 命令停下来的间隙，才由 40 ms / 40 ms 的方波继续闪。
  *
  * 相位拉到 FLASH_HALF 而不是 0，是因为 square() 在 0 处返回"暗"。
  */
static void flash_accent(void)
{
    s_flash_dip = 0u;                /* 写命令盖掉读命令的凹口 */
    s_phase = USBI_LED_FLASH_HALF;
    s_pulse_pending = 1u;            /* 主循环立刻渲染，不等 20 ms 节拍 */
}

/**
  * @brief  烧录闪灯期间的"读命令响应"：在灯上打一个凹口（或补一个亮点）
  *
  * 读**不能**跟写一样只"拉亮"：烧录过程中写命令密集，灯本来就几乎常亮，
  * 再拉亮也看不出变化 —— 实测单寄存器读因此完全没有反馈。
  *
  * 所以让读命令永远落在**当前状态的反面**：
  *   - 灯亮着 → 这一帧压暗（s_flash_dip），出现一个约 20 ms 的凹口；
  *   - 灯暗着 → 立刻点亮一帧。
  * 这样任何一条读都能看到一次变化，而且和写命令的"保持亮"区分得开。
  */
static void flash_read_accent(void)
{
    if (square(s_phase, USBI_LED_FLASH_HALF)) {
        s_flash_dip = 1u;            /* 亮 → 压暗一帧 */
    }
    s_phase = USBI_LED_FLASH_HALF;   /* 之后回到方波的"亮"段 */
    s_pulse_pending = 1u;            /* 主循环立刻渲染，不等 20 ms 节拍 */
}

/**
  * @brief  20 ms 节拍中断（TIM14 更新中断）
  *
  * 注意：全工程只有这一处实现 HAL_TIM_PeriodElapsedCallback，以后再加别的
  * 定时器中断回调时要把这里改成按 Instance 分派。
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM14) {
        s_tick = 1u;
    }
}

/* Exported functions --------------------------------------------------------*/
void usbi_led_init(void)
{
    ws2812_init();

    /* 注意：这里既不重置 s_bus，也不强推 s_state ——
     * ① usbi_init() 会先完成总线探测并调用 usbi_led_set_bus()，初始化后第一次
     *    渲染就应该是对应的模式色；
     * ② main.c 里 Studio_USB_Init() 早于 usbi_init()，主机很可能已经枚举完成
     *    （usbd_configure_done_callback → usbi_led_set_connected(1)），不能覆盖。 */
    if (!s_connected) {
        s_state = USBI_LED_DISCONNECTED;
    } else if (s_state == USBI_LED_DISCONNECTED) {
        s_state = USBI_LED_CONNECTED;
    }
    s_flashing = 0u;
    s_pulse_ticks = 0u;
    s_pulse_pending = 0u;
    s_breathe = 0u;
    s_phase = 0u;
    s_flash_dip = 0u;

    (void)HAL_TIM_Base_Start_IT(&htim14);   /* 20 ms 心跳 */

#if (USBI_LED_MODE == USBI_LED_MODE_CLASSIC)
    /* 档位 1：先亮一次白色"半亮"，确认灯珠工作（老行为，保留） */
    s_color.r = USBI_LED_OFF_R;
    s_color.g = USBI_LED_OFF_G;
    s_color.b = USBI_LED_OFF_B;
    s_level = 64u;
#else
    /* 档位 2/3：上电不点灯。s_color 是静态零初始化，s_level=0 即一帧全黑；
     * 后面主循环按实际状态每 20 ms 渲染一次，该亮的时候自然就亮了。 */
    s_level = 0u;
#endif
    led_render();
}

uint8_t usbi_led_tick_take(void)
{
    if (!s_tick) {
        return 0u;
    }
    s_tick = 0u;
    return 1u;
}

void usbi_led_task(void)
{
    uint8_t drew_pending = 0u;

    /* ---- 命令刚到：立刻出帧（不受 20 ms 节拍限制） ---- */
    if (s_pulse_pending) {
        s_pulse_pending = 0u;
        drew_pending = 1u;
        render_state();
    }

    if (!usbi_led_tick_take()) {
        return;
    }

    /* ---- 呼吸相位：0 → 255 → 0 ---- */
    s_breathe = (uint8_t)(s_breathe + USBI_LED_BREATHE_STEP);

    /* ---- 烧录闪灯相位 ---- */
    s_phase = (uint8_t)(s_phase + 1u);

    /* ---- 档位 3 的"招呼时间"倒数：到 0 之后空闲呼吸就不再出帧 ---- */
    if (s_announce_ticks != 0u) {
        s_announce_ticks--;
    }

    /* ---- 读命令的凹口只保持一个完整节拍 ----
     * 本 pass 里如果刚画过 pending 帧，就不能在同一次里把它抹掉，
     * 否则压暗帧只存在几微秒，灯上根本看不见。 */
    if (!drew_pending) {
        s_flash_dip = 0u;
    }

    /* ---- 脉冲推进：走完整轮（亮 + 暗）就回到呼吸 ---- */
    if (s_pulse_ticks != 0u) {
        s_pulse_ticks--;
        if (s_pulse_ticks == 0u) {
            s_state = s_connected ? USBI_LED_CONNECTED : USBI_LED_DISCONNECTED;
        }
    }

    render_state();
}

void usbi_led_set_state(usbi_led_state_t state)
{
    s_state = (uint8_t)state;
}

uint8_t usbi_led_get_state(void)
{
    return s_state;
}

void usbi_led_set_connected(uint8_t connected)
{
    s_connected = connected ? 1u : 0u;
    if (!s_connected) {
        s_pulse_ticks = 0u;
        s_pulse_pending = 0u;
        s_flashing = 0u;
        s_state = USBI_LED_DISCONNECTED;
        s_announce_ticks = 0u;      /* 拔掉立刻灭，重新枚举时再重新招呼 */
    } else {
        /* 每次"枚举成功"都重新计一遍招呼时间（档位 3 用） */
        s_announce_ticks = USBI_LED_ANNOUNCE_TICKS;
        if (s_state == USBI_LED_DISCONNECTED) {
            s_state = USBI_LED_CONNECTED;
        }
    }
}

void usbi_led_set_flashing(uint8_t flashing)
{
    s_flashing = flashing ? 1u : 0u;
    if (s_flashing) {
        /* 烧录状态优先级最高：放弃正在走的脉冲 */
        s_pulse_ticks = 0u;
        s_pulse_pending = 0u;
        s_state = USBI_LED_FLASHING;
        s_phase = USBI_LED_FLASH_HALF;   /* 从"亮"开始闪，烧录一开始就看得见 */
        s_flash_dip = 0u;
        s_pulse_pending = 1u;
    } else if (s_state == USBI_LED_FLASHING) {
        s_flash_dip = 0u;
        s_state = s_connected ? USBI_LED_CONNECTED : USBI_LED_DISCONNECTED;
    }
}

/**
  * @brief  告知当前总线模式（只改色相：SPI 绿 / I2C 紫）
  *
  * 由 usbi.c 在 usbi_init() 探测完成、以及 usbi_bus_set() 切换后调用。
  * 在 usbi_led_init() 之前调用也是安全的（初始化不会清掉它）。
  */
void usbi_led_set_bus(uint8_t bus)
{
    s_bus = (bus == USBI_BUS_I2C) ? USBI_BUS_I2C : USBI_BUS_SPI;
}

uint8_t usbi_led_get_bus(void)
{
    return s_bus;
}

void usbi_led_notify_write(void)
{
    /* 有 vendor 请求就说明主机在跟我们通信；万一 CONFIGURED 事件没来，
     * 这里也能把灯从"未连接"切到"已连接" */
    s_connected = 1u;

    if (s_flashing) {
        /* 烧录中：任何一条写（2 字节也是）都要有响应 */
        flash_accent();
    } else {
        start_pulse(USBI_LED_WRITING, USBI_LED_WRITE_ON_TICKS,
                    USBI_LED_WRITE_OFF_TICKS, USBI_LED_WRITE_LEVEL);
    }
}

void usbi_led_notify_read(void)
{
    s_connected = 1u;

    if (s_flashing) {
        /* 烧录中：任何一条读都要有响应 —— 但读用"压暗凹口"，不用"拉亮" */
        flash_read_accent();
    } else {
        start_pulse(USBI_LED_READING, USBI_LED_READ_ON_TICKS,
                    USBI_LED_READ_OFF_TICKS, USBI_LED_READ_LEVEL);
    }
}

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
