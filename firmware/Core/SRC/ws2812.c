/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ws2812.c
  * @brief   WS2812(B) 灯珠驱动实现（TIM17_CH1 + DMA1_Channel1）
  *
  *          数据通路：
  *             缓冲区 s_dma_buf[]（每字节一个 PWM 占空值）
  *                 --DMA1_Channel1（内存字节 → 外设半字）--> TIM17->CCR1
  *             TIM17 更新事件（UEV）触发一次 DMA 搬运，于是每个 PWM 周期
  *             输出缓冲区里的下一个占空比。
  *
  *          帧结构（这就是线上真正出现的波形，改代码时别破坏它）：
  *
  *             [帧间]   上一帧结束后数据线保持低电平（约 20 ms），远大于
  *                      WS2812 要求的 >50 µs 复位
  *             [槽 0]   CC1PE=1（预装载）带来的 1 个"空槽"：CCR=0，整周期低电平。
  *                      它紧跟在上面那段 20 ms 低电平后面，和复位电平连成一片，
  *                      不构成一个位，也不影响后面 24 个数据位的对齐。
  *             [槽 1..24]  3 字节 × 8 位，MSB first，线序见 WS2812_ORDER_GRB
  *             [槽 25..83] 59 个 CCR=0 的槽 ≈ 74 µs 复位低电平
  *
  *          ⚠ 帧尾为什么是 59 而不是 60：DMA 的 TC 中断在最后一个 UEV 上触发，
  *          回调里立刻停表，所以第 84 个槽只走了几个计数就被截断 —— 那是复位槽
  *          （本来就是低电平），无损。数据位 24 个一个不少。
  *
  *          注意：mcustudio 生成的 tim.c 把 hdma_tim17_up 的方向配成了
  *          DMA_PERIPH_TO_MEMORY（那是给"输入捕获"用的），本模块在
  *          ws2812_init() 里改成 DMA_MEMORY_TO_PERIPH。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "ws2812.h"
#include "tim.h"

/* Private variables ---------------------------------------------------------*/
/* 一个 bit 一个槽位，帧尾再加 RESET_SLOTS 个 0 */
static uint8_t s_dma_buf[WS2812_LED_COUNT * WS2812_BITS_PER_LED + WS2812_RESET_SLOTS];

/* 灯珠颜色缓存（软件层按 R/G/B 存，show() 时按线序打包） */
static ws2812_crgb_t s_leds[WS2812_LED_COUNT];

/* 一帧正在发送时为 1；此时新的 show() 直接丢帧（WS2812 是单向的，丢帧无害） */
static volatile uint8_t s_busy;

/* Private function prototypes -----------------------------------------------*/
static void ws2812_dma_cplt(DMA_HandleTypeDef *hdma);
static void ws2812_pack_byte(uint32_t *pos, uint8_t value);

/* =========================== 内部实现 =========================== */
/**
  * @brief  DMA 传输完成回调：停计数器并把 CCR 归零，让数据线保持低电平
  *
  *        这里必须放在 DMA 中断里做（毫秒级都不能拖）：TC 之后如果再有几个
  *        UEV，就会把 CCR 之外的槽位也走一遍，帧尾的复位低电平就不好控制。
  */
static void ws2812_dma_cplt(DMA_HandleTypeDef *hdma)
{
    (void)hdma;

    __HAL_TIM_DISABLE(&htim17);                       /* 停计数器 */
    TIM17->CCR1 = 0u;                                 /* CC1PE=1 时写的是预装载寄存器 */
    __HAL_TIM_CLEAR_FLAG(&htim17, TIM_FLAG_UPDATE);   /* 清掉残留 UIF，下一帧的起点才干净 */
    s_busy = 0u;
}

/**
  * @brief  把一个字节按 MSB first 打包成 8 个 PWM 槽位
  */
static void ws2812_pack_byte(uint32_t *pos, uint8_t value)
{
    for (uint8_t i = 0; i < 8u; i++) {
        s_dma_buf[(*pos)++] = (value & 0x80u) ? (uint8_t)WS2812_T1H_CCR
                                              : (uint8_t)WS2812_T0H_CCR;
        value = (uint8_t)(value << 1);
    }
}

/* =========================== 对外接口 =========================== */
void ws2812_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* ① 数据脚 PB9 → TIM17_CH1 复用（tim.c 已配过，这里重配保证本模块可独立使用） */
    gpio.Pin = WS2812_DATA_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF2_TIM17;
    HAL_GPIO_Init(WS2812_DATA_PORT, &gpio);

    /* ② 灯珠供电由板子自己负责，这里不驱动任何电源脚 */
    HAL_Delay(1);

    /* ③ DMA：方向必须是"内存 → 外设"（生成代码里是反的）
     *
     *    优先级给到 VERY_HIGH：一个位只有 1.25 µs（60 个时钟），而 USB 中断
     *    随时可能压上来。DMA 是总线主机、不会被 CPU 打断，但总线仲裁会被
     *    CPU 的突发访问拖后，能给的余量就给满。
     */
    hdma_tim17_up.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_tim17_up.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_tim17_up.Init.MemInc = DMA_MINC_ENABLE;
    hdma_tim17_up.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; /* CCR1 是 16 位 */
    hdma_tim17_up.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;        /* 缓冲区是字节 */
    hdma_tim17_up.Init.Mode = DMA_NORMAL;
    hdma_tim17_up.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    hdma_tim17_up.XferCpltCallback = ws2812_dma_cplt;
    hdma_tim17_up.XferHalfCpltCallback = NULL;
    hdma_tim17_up.XferErrorCallback = NULL;
    if (HAL_DMA_Init(&hdma_tim17_up) != HAL_OK) {
        return;
    }

    /* ④ 启动 PWM 通道（HAL 会顺手打开计数器），随后立刻停掉，等 show() 再放行 */
    __HAL_TIM_SET_COMPARE(&htim17, TIM_CHANNEL_1, 0u);
    (void)HAL_TIM_PWM_Start(&htim17, TIM_CHANNEL_1);
    __HAL_TIM_DISABLE(&htim17);

    /* ⑤ CCR 预装载必须打开（tim.c 里已经开了，这里再显式来一遍，不依赖生成代码）。
     *
     *    为什么不能关：DMA 在 UEV 之后几个时钟才把新占空写进 CCR1，而一个位
     *    只有 60 个时钟。关了预装载，新值会**当场**参与本周期比较：如果这一位
     *    是 0（CCR=17）而 DMA 晚到、CNT 已经越过 17，输出会一直高到陈旧的
     *    CCR=38 才落下 → 这一位被灯珠读成 1。
     *    开着预装载，写入落进影子寄存器，下一个 UEV 才生效：DMA 晚到最多让
     *    上一位重复一次，绝不会把 0 变成 1（不会凭空点出红/橙色）。
     */
    __HAL_TIM_ENABLE_OCxPRELOAD(&htim17, TIM_CHANNEL_1);

    /* ⑥ 更新事件触发 DMA */
    __HAL_TIM_ENABLE_DMA(&htim17, TIM_DMA_UPDATE);

    /* 上电先全灭（灯珠刚上电时里面可能是随机内容） */
    ws2812_fill(0u, 0u, 0u);
    (void)ws2812_show();
}

uint8_t ws2812_busy(void)
{
    return s_busy;
}

/**
  * @brief  把当前颜色缓存打包成一帧并启动 DMA 发送；非阻塞
  * @retval 1 = 这一帧已经上路；0 = 上一帧还没发完（丢帧）或启动失败
  */
uint8_t ws2812_show(void)
{
    uint32_t pos = 0u;

    if (s_busy) {
        /* 兜底：DMA 通道已经不在使能状态，说明这一帧的 TC 中断不会再来了。
         * 此时 s_busy 会被永久占住，灯再也不刷新（表现为"卡在某个颜色"）。
         * 只要有 EN=0 就当场复位，宁可丢一帧。 */
        if ((hdma_tim17_up.Instance->CCR & DMA_CCR_EN) == 0u) {
            if (hdma_tim17_up.State == HAL_DMA_STATE_BUSY) {
                (void)HAL_DMA_Abort(&hdma_tim17_up);
            }
            __HAL_TIM_DISABLE(&htim17);
            TIM17->CCR1 = 0u;
            s_busy = 0u;
        } else {
            return 0u;   /* 上一帧还在发，丢掉这一帧 */
        }
    }

    for (uint16_t i = 0; i < WS2812_LED_COUNT; i++) {
#if WS2812_ORDER_GRB
        ws2812_pack_byte(&pos, s_leds[i].g);
        ws2812_pack_byte(&pos, s_leds[i].r);
#else
        ws2812_pack_byte(&pos, s_leds[i].r);
        ws2812_pack_byte(&pos, s_leds[i].g);
#endif
        ws2812_pack_byte(&pos, s_leds[i].b);
    }
    for (uint16_t i = 0; i < WS2812_RESET_SLOTS; i++) {
        s_dma_buf[pos++] = 0u;   /* 帧尾低电平复位 */
    }

    s_busy = 1u;
    __HAL_TIM_DISABLE(&htim17);

    /* 帧起点做成确定的：CNT 归零 + 清 UIF。
     *
     * 原来这里不清 CNT，表会从上一帧停下的那个计数值接着跑，于是"第一个
     * 空槽"的长度在 1/f_TIM 到 1/800kHz 之间随机（0.02 ~ 1.25 µs）。
     * 对靠"低电平超时"判断帧头的灯珠，这个随机长度本身就是不确定因素。
     * 归零后每一帧的波形完全一致，出问题时才好对着示波器排查。 */
    __HAL_TIM_SET_COUNTER(&htim17, 0u);
    __HAL_TIM_CLEAR_FLAG(&htim17, TIM_FLAG_UPDATE);

    if (HAL_DMA_Start_IT(&hdma_tim17_up, (uint32_t)(void *)s_dma_buf,
                         (uint32_t)&TIM17->CCR1, pos) != HAL_OK) {
        s_busy = 0u;
        return 0u;
    }
    __HAL_TIM_ENABLE(&htim17);
    return 1u;
}

void ws2812_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_fill(r, g, b);
}

void ws2812_set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= WS2812_LED_COUNT) {
        return;
    }
    s_leds[index].r = r;
    s_leds[index].g = g;
    s_leds[index].b = b;
}

void ws2812_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint16_t i = 0; i < WS2812_LED_COUNT; i++) {
        s_leds[i].r = r;
        s_leds[i].g = g;
        s_leds[i].b = b;
    }
}

/**
  * @brief  标准 HSV → RGB（与参考驱动同一套算法，h/s/v 均 0~255）
  */
void ws2812_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, ws2812_crgb_t *out)
{
    uint8_t region, remainder, p, q, t;

    if (out == NULL) {
        return;
    }
    if (s == 0u) {
        out->r = v;
        out->g = v;
        out->b = v;
        return;
    }

    region = (uint8_t)(h / 43u);
    remainder = (uint8_t)((h - (region * 43u)) * 6u);

    p = (uint8_t)((v * (255u - s)) >> 8);
    q = (uint8_t)((v * (255u - ((s * remainder) >> 8))) >> 8);
    t = (uint8_t)((v * (255u - ((s * (255u - remainder)) >> 8))) >> 8);

    switch (region) {
    case 0u: out->r = v; out->g = t; out->b = p; break;
    case 1u: out->r = q; out->g = v; out->b = p; break;
    case 2u: out->r = p; out->g = v; out->b = t; break;
    case 3u: out->r = p; out->g = q; out->b = v; break;
    case 4u: out->r = t; out->g = p; out->b = v; break;
    default: out->r = v; out->g = p; out->b = q; break;
    }
}

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
