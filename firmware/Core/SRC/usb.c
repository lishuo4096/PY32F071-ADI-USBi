/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usb.c
  * @brief   USB 设备初始化入口
  *
  *          mcustudio 生成的模板是 HID 鼠标示例，本文件已把它替换成
  *          "虚拟 USBi"：描述符、vendor 请求处理、总线后端都在 usbi.c 里，
  *          这里只负责注册 + 打开中断。
  *
  *          注意：设备对外必须是"单接口 class 0xFF、零端点"，
  *          HID/CDC 都会强加端点与描述符，所以模板代码全部删掉了。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbd_core.h"
#include "usb_config.h"
#include "usb.h"
#include "usbi.h"
#include "usbi_led.h"

/**
  * @brief CherryUSB 配置完成回调（core 的弱符号，由本文件覆盖）
  */
void usbd_configure_done_callback(void)
{
    /* 主机完成 SET_CONFIGURATION：指示灯从"未连接"切到"已连接" */
    usbi_led_set_connected(1u);
}

/**
  * @brief 由 main() 调用的 USB 初始化（含 NVIC）
  */
void Studio_USB_Init(void)
{
    __HAL_RCC_USB_CLK_ENABLE();

    usbi_usb_init();

    HAL_NVIC_SetPriority(USB_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USB_IRQn);
}

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
