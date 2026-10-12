/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbi_led.h
  * @brief   USBi 状态指示灯（WS2812）—— 用一颗灯珠表达仿真器的工作状态
  *
  *          配色原则：
  *
  *             模式色（只用在"已连接、空闲呼吸"上）：
  *                 SPI = 绿色、I2C = 紫色
  *
  *             数据活动色（读写时不区分总线模式，固定颜色）：
  *                 写入命令 = 蓝色、读出命令 = 青色
  *
  *             烧录程序 = 红色高频闪
  *
  *          状态表：
  *             未连接电脑          白色呼吸
  *             已连接、空闲        模式色呼吸
  *             一条写入命令        蓝色 · 一次完整的"点亮 → 熄灭"
  *             一条读出命令        青色（更暗）· 一次完整的"点亮 → 熄灭"，暗相位更长
  *             正在下载/烧录程序   红色高频闪
  *
  *          写入/读出是"一条命令 = 一个完整脉冲"：命令到来时主循环立刻渲染
  *          点亮帧（微秒级响应，不受 20 ms 节拍限制），随后由 TIM14 节拍把
  *          亮/暗相位推进完，再回到呼吸。脉冲进行中不重新触发，所以连续命令
  *          形成的是稳定节奏，而不是一直亮着。
  *
  *          呼吸节拍由 TIM14 的 20 ms 中断提供，状态机在主循环里跑
  *          （usbi_led_task()），因此不占用 USB 中断的时间。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USBI_LED_H__
#define __USBI_LED_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Exported types ------------------------------------------------------------*/
typedef enum {
    USBI_LED_DISCONNECTED = 0,  /* 未连接电脑：白色呼吸 */
    USBI_LED_CONNECTED,         /* 已连接、无数据：模式色呼吸 */
    USBI_LED_WRITING,           /* 刚收到一条写入命令：一个模式色脉冲 */
    USBI_LED_READING,           /* 刚收到一条读出命令：一个模式色脉冲 */
    USBI_LED_FLASHING           /* 正在下载/烧录程序：橙色高频闪 */
} usbi_led_state_t;

/* Exported define -----------------------------------------------------------*/
/* 呼吸速度：每个节拍变化量（4 → 一个完整呼吸约 2.56 s） */
#define USBI_LED_BREATHE_STEP 4u

/* 一条命令 = 一个完整脉冲：先亮 ON_TICKS 个节拍，再暗 OFF_TICKS 个节拍。
 * 单位都是 20 ms 节拍；脉冲进行中不重新触发（连续命令也不会一直亮）。 */
#define USBI_LED_WRITE_ON_TICKS   2u    /* 亮 40 ms */
#define USBI_LED_WRITE_OFF_TICKS  2u    /* 暗 40 ms → 一条写入命令 = 80 ms 脉冲 */
#define USBI_LED_READ_ON_TICKS    2u    /* 亮 40 ms */
#define USBI_LED_READ_OFF_TICKS   4u    /* 暗 80 ms → 读出脉冲更稀疏，和写入区分 */

/* 脉冲"暗"相位的残余亮度（0 = 完全熄灭） */
#define USBI_LED_DARK_LEVEL 0u

/* 烧录指示灯的高频闪半周期（2 → 亮 40 ms / 暗 40 ms） */
#define USBI_LED_FLASH_HALF 2u

/* 各活动状态的"亮"相位亮度 */
#define USBI_LED_WRITE_LEVEL 127u
#define USBI_LED_READ_LEVEL  85u

/* 模式色：只用在"已连接、空闲呼吸"这一种状态上
 *   SPI = 绿色、I2C = 紫色（亮度按需要调，这里都不打满） */
#define USBI_LED_SPI_R 0u
#define USBI_LED_SPI_G 64u
#define USBI_LED_SPI_B 24u
#define USBI_LED_I2C_R 64u
#define USBI_LED_I2C_G 0u
#define USBI_LED_I2C_B 107u

/* 未连接 = 白色 */
#define USBI_LED_OFF_R 64u
#define USBI_LED_OFF_G 64u
#define USBI_LED_OFF_B 64u

/* 数据活动色：固定颜色，不区分总线模式（读/写靠色相 + 节奏区分）*/
#define USBI_LED_WRITE_R 0u     /* 写入命令 = 蓝色 */
#define USBI_LED_WRITE_G 48u
#define USBI_LED_WRITE_B 127u
#define USBI_LED_READ_R 0u      /* 读出命令 = 青色（更暗、暗相位更长）*/
#define USBI_LED_READ_G 127u
#define USBI_LED_READ_B 127u

/* 烧录 = 红色高频闪 */
#define USBI_LED_BURN_R 127u
#define USBI_LED_BURN_G 8u
#define USBI_LED_BURN_B 0u

/* Exported functions --------------------------------------------------------*/
void    usbi_led_init(void);
void    usbi_led_task(void);              /* 主循环调用 */
void    usbi_led_set_state(usbi_led_state_t state);
uint8_t usbi_led_get_state(void);
void    usbi_led_set_connected(uint8_t connected);
void    usbi_led_set_flashing(uint8_t flashing);
void    usbi_led_set_bus(uint8_t bus);    /* USBI_BUS_SPI / USBI_BUS_I2C：只改色相 */
uint8_t usbi_led_get_bus(void);
void    usbi_led_notify_write(void);      /* 0xB2 处理完后调用（可在 USB 中断里调用） */
void    usbi_led_notify_read(void);       /* 0xB4 处理完后调用（可在 USB 中断里调用） */
uint8_t usbi_led_tick_take(void);         /* 取走一个节拍标志，有则返回 1 */

#ifdef __cplusplus
}
#endif

#endif /* __USBI_LED_H__ */
