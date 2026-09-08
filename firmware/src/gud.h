/*
 * GUD (Generic USB Display) protocol definitions.
 *
 * These are transcribed from the kernel header that defines the wire format,
 * /lib/modules/$(uname -r)/build/include/drm/gud.h (SPDX MIT, Noralf Tronnes).
 * We cannot include that header on the target -- it pulls in <linux/types.h>
 * -- so the structures and request numbers are repeated here. Anything that
 * disagrees with the kernel header is a bug in this file.
 *
 * The whole protocol is: a set of vendor control requests on endpoint 0 that
 * describe the display and set its state, plus one bulk OUT endpoint that
 * carries pixels. There is no other traffic.
 *
 * All multi-byte fields are little-endian, which is also the CPU's byte order,
 * so no swapping is needed anywhere -- but the structures must be packed,
 * because the compiler would otherwise insert padding that is not on the wire.
 */

#ifndef GUD_H
#define GUD_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Structures on the wire                                              */
/* ------------------------------------------------------------------ */

/*
 * Answer to GUD_REQ_GET_DESCRIPTOR. This is what makes the kernel decide the
 * device is a display at all: if the magic does not match, gud_probe() gives
 * up with -ENODEV and the driver never binds.
 */
struct gud_display_descriptor_req {
    uint32_t magic;
#define GUD_DISPLAY_MAGIC                 0x1d50614du
    uint8_t  version;
    uint32_t flags;
#define GUD_DISPLAY_FLAG_STATUS_ON_SET    (1u << 0)
#define GUD_DISPLAY_FLAG_FULL_UPDATE      (1u << 1)
    uint8_t  compression;
#define GUD_COMPRESSION_LZ4               (1u << 0)
    uint32_t max_buffer_size;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
} __attribute__((packed));

struct gud_property_req {
    uint16_t prop;
    uint64_t val;
} __attribute__((packed));

struct gud_display_mode_req {
    uint32_t clock;          /* pixel clock in kHz */
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint32_t flags;
#define GUD_DISPLAY_MODE_FLAG_PREFERRED   (1u << 10)
} __attribute__((packed));

struct gud_connector_descriptor_req {
    uint8_t  connector_type;
#define GUD_CONNECTOR_TYPE_PANEL          0
    uint32_t flags;
#define GUD_CONNECTOR_FLAGS_POLL_STATUS   (1u << 0)
} __attribute__((packed));

/*
 * Sent immediately before every bulk transfer. x/y/width/height say where in
 * the framebuffer the incoming pixels belong -- this is the damage rectangle.
 * `length` is the size after decompression, `compressed_length` the number of
 * bytes that will actually arrive on the bulk endpoint.
 */
struct gud_set_buffer_req {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t length;
    uint8_t  compression;
    uint32_t compressed_length;
} __attribute__((packed));

/*
 * Sent on every modeset. The properties array is variable length; its size is
 * whatever is left over after the fixed part.
 */
struct gud_state_req {
    struct gud_display_mode_req mode;
    uint8_t format;
    uint8_t connector;
    /* struct gud_property_req properties[]; follows */
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Control requests                                                    */
/* ------------------------------------------------------------------ */

/*
 * Every one of these is a vendor request to the interface:
 *   bmRequestType = 0xC0 (IN) or 0x40 (OUT), i.e. vendor | interface
 *   bRequest      = one of the values below
 *   wValue        = connector index, for the connector requests; 0 otherwise
 *   wIndex        = our interface number
 * See gud_usb_control_msg() in drivers/gpu/drm/gud/gud_drv.c.
 */

#define GUD_REQ_GET_STATUS                       0x00
  #define GUD_STATUS_OK                          0x00
  #define GUD_STATUS_BUSY                        0x01
  #define GUD_STATUS_REQUEST_NOT_SUPPORTED       0x02
  #define GUD_STATUS_PROTOCOL_ERROR              0x03
  #define GUD_STATUS_INVALID_PARAMETER           0x04
  #define GUD_STATUS_ERROR                       0x05

#define GUD_REQ_GET_DESCRIPTOR                   0x01

#define GUD_REQ_GET_FORMATS                      0x40
  #define GUD_PIXEL_FORMAT_R1                    0x01
  #define GUD_PIXEL_FORMAT_R8                    0x08
  #define GUD_PIXEL_FORMAT_XRGB1111              0x20
  #define GUD_PIXEL_FORMAT_RGB332                0x30
  #define GUD_PIXEL_FORMAT_RGB565                0x40
  #define GUD_PIXEL_FORMAT_RGB888                0x50
  #define GUD_PIXEL_FORMAT_XRGB8888              0x80
  #define GUD_PIXEL_FORMAT_ARGB8888              0x81

#define GUD_REQ_GET_PROPERTIES                   0x41

#define GUD_REQ_GET_CONNECTORS                   0x50
#define GUD_REQ_GET_CONNECTOR_PROPERTIES         0x51
#define GUD_REQ_GET_CONNECTOR_TV_MODE_VALUES     0x52
#define GUD_REQ_SET_CONNECTOR_FORCE_DETECT       0x53
#define GUD_REQ_GET_CONNECTOR_STATUS             0x54
  #define GUD_CONNECTOR_STATUS_DISCONNECTED      0x00
  #define GUD_CONNECTOR_STATUS_CONNECTED         0x01
  #define GUD_CONNECTOR_STATUS_UNKNOWN           0x02
#define GUD_REQ_GET_CONNECTOR_MODES              0x55
#define GUD_REQ_GET_CONNECTOR_EDID               0x56

#define GUD_REQ_SET_BUFFER                       0x60
#define GUD_REQ_SET_STATE_CHECK                  0x61
#define GUD_REQ_SET_STATE_COMMIT                 0x62
#define GUD_REQ_SET_CONTROLLER_ENABLE            0x63
#define GUD_REQ_SET_DISPLAY_ENABLE               0x64

#endif /* GUD_H */
