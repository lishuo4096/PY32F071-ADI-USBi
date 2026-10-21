/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbi_led.h
  * @brief   USBi 状态指示灯（WS2812）—— 用一颗灯珠表达仿真器的工作状态
  *
  *          配色原则：
  *
  *             模式色（只用在"已连接、空闲呼吸"上）：
  *                 SPI = 绿色（纯绿，B 恒为 0）、I2C = 紫色
  *
  *             数据活动色（读写时不区分总线模式，固定颜色）：
  *                 写入命令 = 蓝色、读出命令 = 青色
  *
  *             烧录程序 = 紫色高频闪
  *
  *          状态表（模式 1，即经典行为）：
  *             未连接电脑          白色呼吸
  *             已连接、空闲        模式色呼吸（绿色）
  *             一条写入命令        蓝色 · 一次完整的"点亮 → 熄灭"
  *             一条读出命令        青色（更暗）· 一次完整的"点亮 → 熄灭"，暗相位更长
  *             正在下载/烧录程序   紫色高频闪
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

/* ========================= 编译期显示模式 =========================
 *
 * 灯珠显示什么，在**编译时**选一档。改这里重新编译即可，运行时不可切换：
 *
 *   1 = 经典（改动前的老行为）
 *         未枚举：白色呼吸
 *         已枚举：模式色呼吸（SPI 绿 / I2C 紫）
 *         读/写命令：蓝色/青色脉冲
 *         烧录：紫色高频闪
 *
 *   2 = 插入才亮（当前默认）
 *         未枚举：**不亮**（完全熄灭）
 *         已枚举：模式色呼吸，读/写脉冲、烧录闪照旧
 *
 *   3 = 只有活动才亮（枚举时先打个招呼）
 *         未枚举：**不亮**（完全熄灭）
 *         刚枚举上：模式色呼吸 USBI_LED_ANNOUNCE_MS（默认 3 s），到点自动熄灭
 *         之后：**不亮**，只有活动才亮
 *         任何一条读/写命令 → 亮一次脉冲（读 = 青色、写 = 蓝色）
 *         下载/烧录 → 紫色高频闪
 *         —— 适合做"安静的仿真器"：不通信的时候完全黑，一眼就能看出
 *            总线上有没有活动，也适合拿到不被灯干扰的暗环境；
 *            开机那 3 秒呼吸是"我认到主机了"的确认信号（未枚举时不呼吸，
 *            所以黑着插进去、亮起来 = 枚举成功，这个信息不会丢）
 *
 * 想改成别的档位：把下面 USBI_LED_MODE 的默认值改掉，
 * 或者在 EIDE 的预处理宏里加 -DUSBI_LED_MODE=3（后者优先，不用改文件）。
 */
#define USBI_LED_MODE_CLASSIC        1u
#define USBI_LED_MODE_AFTER_ENUM     2u
#define USBI_LED_MODE_ACTIVITY_ONLY  3u

/* 档位 3 专属：枚举成功后先呼吸多久（毫秒），之后彻底熄灭。
 * 换成节拍数就是 3000 / 20 = 150 拍。 */
#ifndef USBI_LED_ANNOUNCE_MS
#define USBI_LED_ANNOUNCE_MS 3000u
#endif
#define USBI_LED_ANNOUNCE_TICKS ((uint16_t)((USBI_LED_ANNOUNCE_MS + 19u) / 20u))

#ifndef USBI_LED_MODE
#define USBI_LED_MODE USBI_LED_MODE_ACTIVITY_ONLY
#endif

#if (USBI_LED_MODE != USBI_LED_MODE_CLASSIC) && \
    (USBI_LED_MODE != USBI_LED_MODE_AFTER_ENUM) && \
    (USBI_LED_MODE != USBI_LED_MODE_ACTIVITY_ONLY)
#error "USBI_LED_MODE 只能是 1 / 2 / 3（见 usbi_led.h 顶部说明）"
#endif
/* ================================================================= */

/* Exported types ------------------------------------------------------------*/
typedef enum {
    USBI_LED_DISCONNECTED = 0,  /* 未连接电脑：白色呼吸（模式 2/3 时灭） */
    USBI_LED_CONNECTED,         /* 已连接、无数据：模式色呼吸 */
    USBI_LED_WRITING,           /* 刚收到一条写入命令：一个模式色脉冲 */
    USBI_LED_READING,           /* 刚收到一条读出命令：一个模式色脉冲 */
    USBI_LED_FLASHING           /* 正在下载/烧录程序：紫色高频闪 */
} usbi_led_state_t;

/* Exported define -----------------------------------------------------------*/
/* 呼吸速度：每个节拍变化量（4 → 一个完整呼吸约 2.56 s） */
#define USBI_LED_BREATHE_STEP 4u

/* 呼吸最亮能到多少（0~255）。
 *
 * 默认 255 = 呼吸峰值为全亮的 ~99%（原行为）。
 * 如果实机在**呼吸到峰值**时偶尔闪一下杂色（典型是橙色），先把这里降到
 * 200 试：峰值电流小了、灯珠自己的内部振荡器的时序漂移也小，能立刻判断
 * 是不是供电/电平余量的问题。
 * 原理：峰值那一帧的绿字节 = 0b00111111（连着 6 个 1），一旦某一位错位，
 * 最低那个 1 会掉进红色字节的 MSB，红 = 0x80 → 看着就是橙色。所以这种
 * "只在峰值闪一下"的现象对亮度极其敏感，降峰值是等效的验证手段。 */
#ifndef USBI_LED_PEAK_LEVEL
#define USBI_LED_PEAK_LEVEL 127u
#endif

/* 一条命令 = 一个完整脉冲：先亮 ON_TICKS 个节拍，再暗 OFF_TICKS 个节拍。
 * 单位都是 20 ms 节拍；脉冲进行中不重新触发（连续命令也不会一直亮）。 */
#define USBI_LED_WRITE_ON_TICKS   2u    /* 亮 40 ms */
#define USBI_LED_WRITE_OFF_TICKS  2u    /* 暗 40 ms → 一条写入命令 = 80 ms 脉冲 */
#define USBI_LED_READ_ON_TICKS    2u    /* 亮 40 ms */
#define USBI_LED_READ_OFF_TICKS   4u    /* 暗 80 ms → 读出脉冲更稀疏，和写入区分 */

/* 脉冲"暗"相位的残余亮度（0 = 完全熄灭） */
#define USBI_LED_DARK_LEVEL 0u

/* 烧录指示灯的高频闪半周期（2 → 亮 40 ms / 暗 40 ms）
 *
 * 烧录过程中的**每一条**读/写命令都会在灯上留下一次可见响应，但两者表现
 * 故意做得不一样（见 usbi_led.c 的 flash_accent / flash_read_accent）：
 *   - 写命令 → 相位拉回"亮"：命令落在暗相位时灯马上变亮；
 *             命令密集（正在搬 program data）→ 灯基本保持亮；
 *   - 读命令 → 打一个"凹口"：灯亮着就压暗一帧（约 20 ms），灯暗着就补亮
 *             一帧。读永远落在当前状态的反面，所以任何一条读都看得见，也
 *             不会被"一直亮着"的写流量淹没；
 *   - 命令之间的间隙 → 由上面这个 40/40 ms 方波继续闪。
 * 想让方波占主导（哪怕命令很密也坚持闪），把这个值调大即可。 */
#define USBI_LED_FLASH_HALF 2u

/* 各活动状态的"亮"相位亮度 */
#define USBI_LED_WRITE_LEVEL 127u
#define USBI_LED_READ_LEVEL  85u

/* 模式色：只用在"已连接、空闲呼吸"这一种状态上
 *   SPI = 绿色、I2C = 紫色（亮度按需要调，这里都不打满）
 *
 * 绿色**不掺蓝**（B = 0）：读命令用的青色是 (0,127,127)，绿里只要带一点
 * 蓝分量，呼吸到中低亮度时就分不清"空闲呼吸"和"正在读"了。 */
#define USBI_LED_SPI_R 0u
#define USBI_LED_SPI_G 64u
#define USBI_LED_SPI_B 0u
#define USBI_LED_I2C_R 64u
#define USBI_LED_I2C_G 0u
#define USBI_LED_I2C_B 107u

/* 未连接 = 白色 */
#define USBI_LED_OFF_R 64u
#define USBI_LED_OFF_G 64u
#define USBI_LED_OFF_B 64u

/* 数据活动色：固定颜色，不区分总线模式（读/写靠色相 + 节奏区分）*/
#define USBI_LED_WRITE_R 0u     /* 写入命令 = 蓝色 */
#define USBI_LED_WRITE_G 0u
#define USBI_LED_WRITE_B 127u
#define USBI_LED_READ_R 64u      /* 读出命令 = 紫色（更暗暗相位更长）*/
#define USBI_LED_READ_G 0u
#define USBI_LED_READ_B 96u

/* 烧录 = 青色高频闪 */
#define USBI_LED_BURN_R 0u
#define USBI_LED_BURN_G 96u
#define USBI_LED_BURN_B 64u

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
