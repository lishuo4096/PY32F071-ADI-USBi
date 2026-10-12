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
  */
static void ws2812_dma_cplt(DMA_HandleTypeDef *hdma)
{
    (void)hdma;

    __HAL_TIM_DISABLE(&htim17);   /* 停计数器：输出停在 CCR1 = 0 对应的低电平 */
    TIM17->CCR1 = 0u;
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

    /* ③ DMA：方向必须是"内存 → 外设"（生成代码里是反的） */
    hdma_tim17_up.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_tim17_up.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_tim17_up.Init.MemInc = DMA_MINC_ENABLE;
    hdma_tim17_up.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; /* CCR1 是 16 位 */
    hdma_tim17_up.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;        /* 缓冲区是字节 */
    hdma_tim17_up.Init.Mode = DMA_NORMAL;
    hdma_tim17_up.Init.Priority = DMA_PRIORITY_HIGH;
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

    /* ⑤ 更新事件触发 DMA */
    __HAL_TIM_ENABLE_DMA(&htim17, TIM_DMA_UPDATE);

    /* 上电先全灭（灯珠刚上电时里面可能是随机内容） */
    ws2812_fill(0u, 0u, 0u);
    ws2812_show();
}

uint8_t ws2812_busy(void)
{
    return s_busy;
}

/**
  * @brief  把当前颜色缓存打包成一帧并启动 DMA 发送；非阻塞
  */
void ws2812_show(void)
{
    uint32_t pos = 0u;

    if (s_busy) {
        return;   /* 上一帧还在发，丢掉这一帧 */
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
    TIM17->CCR1 = 0u;

    if (HAL_DMA_Start_IT(&hdma_tim17_up, (uint32_t)(void *)s_dma_buf,
                         (uint32_t)&TIM17->CCR1, pos) != HAL_OK) {
        s_busy = 0u;
        return;
    }
    __HAL_TIM_ENABLE(&htim17);
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
