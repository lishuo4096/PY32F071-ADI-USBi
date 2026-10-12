/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBD_CORE_H
#define USBD_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "usb_config.h"
#include "usb_util.h"
#include "usb_errno.h"
#include "usb_def.h"
#include "usb_list.h"
#include "usb_mem.h"
#include "usb_log.h"
#include "usb_dc.h"

enum usbd_event_type {
    /* USB DCD IRQ */
    USBD_EVENT_ERROR,        /** USB error reported by the controller */
    USBD_EVENT_RESET,        /** USB reset */
    USBD_EVENT_SOF,          /** Start of Frame received */
    USBD_EVENT_CONNECTED,    /** USB connected*/
    USBD_EVENT_DISCONNECTED, /** USB disconnected */
    USBD_EVENT_SUSPEND,      /** USB connection suspended by the HOST */
    USBD_EVENT_RESUME,       /** USB connection resumed by the HOST */

    /* USB DEVICE STATUS */
    USBD_EVENT_CONFIGURED,    /** USB configuration done */
    USBD_EVENT_SET_INTERFACE, /** USB interface selected */
    USBD_EVENT_UNKNOWN
};

typedef int (*usbd_request_handler)(struct usb_setup_packet *setup, uint8_t **data, uint32_t *len);
typedef void (*usbd_endpoint_callback)(uint8_t ep, uint32_t nbytes);
typedef void (*usbd_notify_handler)(uint8_t event, void *arg);

/* ===== 本工程扩展：EP0 OUT 大块写"流式接收" =====
 * 官方 USBi 的写请求可以长达十几 KB（例如烧 EEPROM 时把整幅镜像写进 DM0），
 * 这种长度既放不进 CONFIG_USBDEV_REQUEST_BUFFER_LEN，也放不进 PY32F071 的 16 KB SRAM。
 * 注册分块缓冲后，凡是 wLength 超过 CONFIG_USBDEV_REQUEST_BUFFER_LEN 的 OUT 传输
 * 会按块交给回调处理（每块 ≤ buf_size 字节），而不是整块缓存或直接 STALL。
 * 回调返回非 0 表示出错，core 会 STALL 该传输。
 * 未注册（或 wLength 不超限）时行为与原来完全一致。 */
typedef int (*usbd_ep0_out_stream_handler)(struct usb_setup_packet *setup, uint8_t *data,
                                           uint32_t len, uint32_t offset, uint32_t total);
void usbd_ep0_out_stream_config(uint8_t *buf, uint32_t buf_size, usbd_ep0_out_stream_handler cb);

extern usb_slist_t usbd_intf_head;

struct usbd_endpoint {
    uint8_t ep_addr;
    usbd_endpoint_callback ep_cb;
};

struct usbd_interface {
    usb_slist_t list;
    usbd_request_handler class_interface_handler;
    usbd_request_handler class_endpoint_handler;
    usbd_request_handler vendor_handler;
    usbd_notify_handler notify_handler;
    const uint8_t *hid_descriptor;
    const uint8_t *hid_report_descriptor;
    uint32_t hid_report_descriptor_len;
    uint8_t intf_num;
};

void usbd_desc_register(const uint8_t *desc);
void usbd_msosv1_desc_register(struct usb_msosv1_descriptor *desc);
void usbd_msosv2_desc_register(struct usb_msosv2_descriptor *desc);
void usbd_bos_desc_register(struct usb_bos_descriptor *desc);

void usbd_add_interface(struct usbd_interface *intf);
void usbd_add_endpoint(struct usbd_endpoint *ep);

bool usb_device_is_configured(void);
void usbd_configure_done_callback(void);
int usbd_initialize(void);
int usbd_deinitialize(void);

#ifdef __cplusplus
}
#endif

#endif /* USBD_CORE_H */
