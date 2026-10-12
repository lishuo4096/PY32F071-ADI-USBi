/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ws2812.h
  * @brief   WS2812(B) 灯珠驱动 —— TIM17_CH1(PB9) PWM + DMA1_Channel1
  *
  *            - 一个 bit = 一个 PWM 周期（800 kHz = 1.25 µs）
  *              T0H ≈ 0.35 µs、T1H ≈ 0.79 µs，靠 CCR 值区分 0/1
  *            - 用 DMA（内存字节 → CCR1 半字）在每次更新事件时自动装载下一个
  *              CCR，CPU 只负责准备缓冲区；缓冲区发完才允许下一帧
  *            - 帧尾补足低电平当 reset（WS2812 要求 > 50 µs）
  *
  *          时序换算（PY32F071 @48 MHz、TIM17 ARR=59 → 60 计数 = 1.25 µs）：
  *              T0H = 17 计数 ≈ 0.354 µs、T1H = 38 计数 ≈ 0.792 µs
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __WS2812_H__
#define __WS2812_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Exported define -----------------------------------------------------------*/
/* 灯珠数量（串联时只改这一个宏） */
#ifndef WS2812_LED_COUNT
#define WS2812_LED_COUNT 1u
#endif

#define WS2812_BITS_PER_LED 24u

/* CCR 值（800 kHz 下的占空计数） */
#define WS2812_T0H_CCR     17u
#define WS2812_T1H_CCR     38u

/* 帧尾低电平槽位数：60 × 1.25 µs = 75 µs > 50 µs */
#define WS2812_RESET_SLOTS 60u

/* 数据脚：PB9 = TIM17_CH1（tim.c 的 MspInit 里已配成 AF2） */
#define WS2812_DATA_PORT GPIOB
#define WS2812_DATA_PIN  GPIO_PIN_9

/* 灯珠供电由板子自己负责，PB8 不再由单片机驱动 */

/* 灯珠线上的字节顺序（决定颜色对不对，不决定亮度）：
 *   1 = GRB（WS2812B / SK6812 绝大多数型号的出厂顺序，默认）
 *   0 = RGB（少数批次的灯珠是 RGB 顺序）
 * 现象对照：命令"纯绿"却看到红/橙 → 说明灯珠是 RGB 顺序，把这个宏改成 0。 */
#ifndef WS2812_ORDER_GRB
#define WS2812_ORDER_GRB 1
#endif

/* Exported types ------------------------------------------------------------*/
/* 颜色结构体内部统一按 R/G/B 存，线上顺序在 ws2812_show() 里按上面的宏打包 */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ws2812_crgb_t;

/* Exported functions --------------------------------------------------------*/
void    ws2812_init(void);
uint8_t ws2812_busy(void);
void    ws2812_show(void);
void    ws2812_set_all(uint8_t r, uint8_t g, uint8_t b);
void    ws2812_set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b);
void    ws2812_fill(uint8_t r, uint8_t g, uint8_t b);
void    ws2812_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, ws2812_crgb_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __WS2812_H__ */
