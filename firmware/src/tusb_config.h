#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

/*
 * TinyUSB configuration for the STM32G474 GUD display.
 *
 * The G4's USB peripheral is the "fsdev" style: a full-speed device controller
 * with a dedicated 1024-byte packet memory area that the USB engine and the
 * CPU both see. Every endpoint buffer is carved out of that 1024 bytes. Our
 * budget here is small -- endpoint 0 takes 64 in and 64 out, and the GUD bulk
 * OUT endpoint takes another 64 -- so packet memory is not a constraint for
 * this design, but it is why the bulk endpoint is 64 bytes and not larger.
 * (64 is the full-speed maximum for bulk anyway.)
 */

#define CFG_TUSB_MCU              OPT_MCU_STM32G4
#define CFG_TUSB_OS               OPT_OS_NONE
#define CFG_TUSB_DEBUG            0

#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED

/* TinyUSB 0.21 wants the root-hub port role declared before the zero-argument
 * tusb_init() can be used. The G4 has a single device-only full-speed port. */
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* Control endpoint. 64 is the full-speed maximum. */
#define CFG_TUD_ENDPOINT0_SIZE    64

/*
 * HID is the only built-in class in use.
 *
 * CFG_TUD_VENDOR in particular stays 0: TinyUSB's vendor class would copy bulk
 * data through its own FIFO, costing a third 40 KB buffer we cannot afford and
 * giving no way to direct a specific transfer into a specific buffer. The GUD
 * interface is served by our own class driver in gud_usbd.c instead, hooked in
 * through usbd_app_driver_get_cb(). Application drivers are offered each
 * interface before the built-in ones, so our open() has to decline anything
 * that is not vendor-specific -- that is what lets interface 1 fall through to
 * the HID driver below.
 */
#define CFG_TUD_CDC               0
#define CFG_TUD_HID               1
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

/* A gamepad report from TUD_HID_REPORT_DESC_GAMEPAD() is 11 bytes: six 8-bit
 * axes, a packed hat/button byte and a 32-bit button field. 16 rounds it up
 * with room to spare and costs 16 bytes of USB packet memory. */
#define CFG_TUD_HID_EP_BUFSIZE    16

#endif /* _TUSB_CONFIG_H_ */
