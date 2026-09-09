/*
 * USB descriptors for the GUD display.
 *
 * The single most important line in this file is the VID/PID. The kernel's gud
 * driver does not probe by class alone -- its device table lists exactly three
 * vendor/product pairs, each additionally requiring interface class 0xFF:
 *
 *   $ modinfo gud | grep alias
 *   alias: usb:v1D50p614Dd*dc*dsc*dp*icFFisc*ip*in*
 *   alias: usb:v16D0p10A9d*dc*dsc*dp*icFFisc*ip*in*
 *   alias: usb:v1209p4FB3d*dc*dsc*dp*icFFisc*ip*in*
 *
 * Anything else enumerates perfectly and is then ignored.
 *
 * We use 1D50:614D, not 1209:4FB3. Both are in the table above on a current
 * kernel, but 1209:4FB3 is a recent addition: it is absent from 6.18-LTS and
 * from 7.1.8, both of which list only 1D50:614D and 16D0:10A9. Measured on a
 * 7.1.8 laptop -- the device enumerated, the gud module never loaded, and the
 * panel sat on its splash screen. 1D50:614D is the original Openmoko/pid.codes
 * allocation for a GUD display and has been in the driver since it was merged,
 * so it is the portable choice. There is no other difference: the driver treats
 * all three identically once bound.
 *
 * Interface class 0xFF (vendor-specific) is not a shortcut either: GUD is raw
 * bulk plus vendor control requests, so there is no standard class that
 * describes it.
 */

#include <string.h>
#include "tusb.h"

#define USB_VID 0x1D50u    /* Openmoko / pid.codes  */
#define USB_PID 0x614Du    /* GUD display, all kernels */
#define USB_BCD 0x0200u    /* USB 2.0, full speed */

/* ------------------------------------------------------------------ */
/* Device descriptor                                                   */
/* ------------------------------------------------------------------ */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    /*
     * Class 0 at the device level means "look in the interface descriptors",
     * and it stays 0 here even though this is now a composite device.
     *
     * The obvious thing to copy from phase 2 would be
     * Misc/Common/IAD (0xEF/0x02/0x01), which is what a CDC+HID device needs.
     * It is not needed here, and claiming it without cause would be wrong.
     *
     * An Interface Association Descriptor exists to say "these N *consecutive*
     * interfaces are one function". CDC needs it because a serial port is two
     * interfaces -- a control interface and a data interface -- that must be
     * bound by one driver; without the IAD (and the device class that tells the
     * host to look for it) the host would bind the first interface as the whole
     * device and the second function would never appear.
     *
     * This device has two functions of one interface each: the GUD display on
     * interface 0 and the gamepad on interface 1. Nothing needs grouping, so
     * there is nothing for an IAD to say. Linux binds interface drivers
     * per-interface from the configuration descriptor regardless of
     * bDeviceClass, and the gud driver's own match string does not care:
     *
     *     usb:v1D50p614Dd*dc*dsc*dp*icFFisc*ip*in*
     *                     ^^^ device class is a wildcard
     *                                    ^^^^ interface class must be FF
     *
     * Verified on hardware: both drivers bind with bDeviceClass = 0.
     */
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *) &desc_device;
}

/* ------------------------------------------------------------------ */
/* Configuration descriptor                                            */
/* ------------------------------------------------------------------ */

/*
 * The GUD part is written out byte by byte rather than with TinyUSB's TUD_*
 * macros, because there is no macro for "vendor interface with a single bulk
 * OUT endpoint" -- TUD_VENDOR_DESCRIPTOR insists on an IN endpoint too, which
 * we have no use for and which would waste packet memory. Spelling it out also
 * makes what goes on the wire visible, which is the point of the exercise.
 *
 * The HID part does have a macro that fits exactly, so it uses one.
 */

/*
 * Interface numbers must run from zero with no gaps. The GUD interface is
 * first because it is the reason the board exists; the gamepad rides along.
 */
enum { ITF_NUM_GUD = 0, ITF_NUM_HID, ITF_NUM_TOUCH, ITF_NUM_TOTAL };

/*
 * Endpoint addresses. Bit 7 set means IN (device to host).
 *
 * On the G4's fsdev peripheral every endpoint buffer is carved out of a single
 * 1024-byte packet memory area, so the budget is worth stating: endpoint 0
 * takes 64 in and 64 out, the GUD bulk OUT another 64, and the two HID
 * interrupt IN endpoints 16 each. That is 240 bytes of 1024 -- packet memory
 * is nowhere near a constraint for this design.
 */
#define EPNUM_GUD_OUT   0x01    /* endpoint 1, OUT (bit 7 clear) */
#define EPNUM_HID_IN    0x82    /* endpoint 2, IN  -- gamepad    */
#define EPNUM_TOUCH_IN  0x83    /* endpoint 3, IN  -- touch      */
#define GUD_EP_SIZE     64      /* the full-speed maximum for bulk */

/*
 * Gamepad report descriptor. TinyUSB's template gives the layout Linux's
 * generic HID driver understands: two 8-bit axis pairs, a 4-bit hat switch and
 * 32 buttons. usbhid binds it and exposes js/event nodes with no custom host
 * driver, which is the whole point of using HID rather than something bespoke.
 */
static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GAMEPAD(),
};

/*
 * Touch screen report descriptor, written out by hand.
 *
 * TinyUSB has no template for a digitizer, and the layout matters: Linux picks
 * the host driver from the application usage at the top. A Digitizer / Touch
 * Screen collection that also reports Contact Identifier and Contact Count is
 * what hid-multitouch binds to, and hid-multitouch is what sets
 * INPUT_PROP_DIRECT -- the property libinput uses to tell a touch screen from
 * a touchpad. Without it the panel would come up as a pointer device and drag
 * the cursor on the primary screen instead of acting on its own.
 *
 * Only one contact is declared. The panel is resistive, and a resistive panel
 * physically cannot resolve two fingers: it reports a single voltage per axis,
 * which for two touches lands somewhere between them.
 *
 * X and Y are reported in a 0..4095 logical range rather than 0..319 / 0..239.
 * The panel's own resolution is 12 bits, so this keeps the full precision of
 * the ADC and lets the host scale to whatever the output size happens to be.
 *
 * The report this produces is 7 bytes:
 *
 *      byte 0   bit 0    tip switch (1 = touching)
 *               bits 1-7 padding
 *      byte 1   contact identifier (always 0)
 *      bytes 2-3 X, little endian, 0..4095
 *      bytes 4-5 Y, little endian, 0..4095
 *      byte 6   contact count (0 or 1)
 */
static const uint8_t desc_touch_report[] = {
    0x05, 0x0D,                    /* Usage Page (Digitizers)             */
    0x09, 0x04,                    /* Usage (Touch Screen)                */
    0xA1, 0x01,                    /* Collection (Application)            */
      0x09, 0x22,                  /*   Usage (Finger)                    */
      0xA1, 0x02,                  /*   Collection (Logical)              */
        0x09, 0x42,                /*     Usage (Tip Switch)              */
        0x15, 0x00,                /*     Logical Minimum (0)             */
        0x25, 0x01,                /*     Logical Maximum (1)             */
        0x75, 0x01,                /*     Report Size (1)                 */
        0x95, 0x01,                /*     Report Count (1)                */
        0x81, 0x02,                /*     Input (Data,Var,Abs)            */
        0x95, 0x07,                /*     Report Count (7)                */
        0x81, 0x03,                /*     Input (Cnst,Var,Abs) - padding  */
        0x09, 0x51,                /*     Usage (Contact Identifier)      */
        0x25, 0x7F,                /*     Logical Maximum (127)           */
        0x75, 0x08,                /*     Report Size (8)                 */
        0x95, 0x01,                /*     Report Count (1)                */
        0x81, 0x02,                /*     Input (Data,Var,Abs)            */
        0x05, 0x01,                /*     Usage Page (Generic Desktop)    */
        0x15, 0x00,                /*     Logical Minimum (0)             */
        0x26, 0xFF, 0x0F,          /*     Logical Maximum (4095)          */
        0x75, 0x10,                /*     Report Size (16)                */
        0x95, 0x01,                /*     Report Count (1)                */
        0x09, 0x30,                /*     Usage (X)                       */
        0x81, 0x02,                /*     Input (Data,Var,Abs)            */
        0x09, 0x31,                /*     Usage (Y)                       */
        0x81, 0x02,                /*     Input (Data,Var,Abs)            */
      0xC0,                        /*   End Collection                    */
      0x05, 0x0D,                  /*   Usage Page (Digitizers)           */
      0x09, 0x54,                  /*   Usage (Contact Count)             */
      0x25, 0x7F,                  /*   Logical Maximum (127)             */
      0x75, 0x08,                  /*   Report Size (8)                   */
      0x95, 0x01,                  /*   Report Count (1)                  */
      0x81, 0x02,                  /*   Input (Data,Var,Abs)              */
      0x09, 0x55,                  /*   Usage (Contact Count Maximum)     */
      0x25, 0x01,                  /*   Logical Maximum (1)               */
      0xB1, 0x02,                  /*   Feature (Data,Var,Abs)            */
    0xC0                           /* End Collection                      */
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return (instance == 0) ? desc_hid_report : desc_touch_report;
}

/*
 * GET_REPORT.
 *
 * The gamepad answers nothing. The touch screen must answer one feature
 * report: hid-multitouch reads Contact Count Maximum during probe to size its
 * slot table, and a device that does not answer it is rejected. One byte,
 * value 1, because the panel is resistive and resolves a single contact.
 */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void) report_id;

    /*
     * Contact Count Maximum, read by hid-multitouch while it probes.
     *
     * Note there is no length test here. The host asks for this with
     * wLength = 0 first, and returning 0 from this callback is fatal:
     * hidd_control_xfer_cb() does TU_ASSERT(xferlen > 0), and TU_ASSERT
     * expands to a BKPT that fires only when a debugger is attached. The
     * board therefore worked perfectly in normal use and halted the instant
     * probe-rs connected, which is a memorable way to lose an evening.
     *
     * Answering with one byte is safe either way: tud_control_xfer() clamps
     * the data stage to wLength, so a zero-length request still produces a
     * zero-length data stage.
     */
    if (report_type == HID_REPORT_TYPE_FEATURE && instance == 1) {
        buffer[0] = 1;
        return 1;
    }

    /*
     * Any other report fetched over the control pipe. Nothing needs these:
     * button and touch state travels on its own interrupt endpoint. They are
     * answered with a zeroed report of the declared size, for the same reason
     * as above: this callback must never return zero.
     */
    uint16_t len = (instance == 0) ? (uint16_t) sizeof(hid_gamepad_report_t)
                                   : 7u;    /* touch report, see desc_touch_report */
    if (reqlen && len > reqlen)
        len = reqlen;
    if (len == 0)
        len = 1;

    memset(buffer, 0, len);
    return len;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance; (void) report_id; (void) report_type;
    (void) buffer; (void) bufsize;
}

#define CONFIG_TOTAL_LEN (9 + 9 + 7 + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t desc_configuration[] = {
    /* ---- Configuration descriptor ---- */
    9,                              /* bLength                             */
    TUSB_DESC_CONFIGURATION,        /* bDescriptorType                     */
    U16_TO_U8S_LE(CONFIG_TOTAL_LEN),/* wTotalLength                        */
    ITF_NUM_TOTAL,                  /* bNumInterfaces                      */
    1,                              /* bConfigurationValue                 */
    0,                              /* iConfiguration (no string)          */
    0x80,                           /* bmAttributes: bus powered.
                                     * Bit 7 is reserved and must be 1.    */
    50,                             /* bMaxPower, in 2 mA units -> 100 mA  */

    /* ---- Interface 0: the GUD display ---- */
    9,                              /* bLength                             */
    TUSB_DESC_INTERFACE,            /* bDescriptorType                     */
    ITF_NUM_GUD,                    /* bInterfaceNumber                    */
    0,                              /* bAlternateSetting                   */
    1,                              /* bNumEndpoints                       */
    TUSB_CLASS_VENDOR_SPECIFIC,     /* bInterfaceClass = 0xFF -- required
                                     * by the gud driver's device table    */
    0,                              /* bInterfaceSubClass                  */
    0,                              /* bInterfaceProtocol                  */
    4,                              /* iInterface -> "MiSKo3 GUD"          */

    /* ---- Endpoint descriptor: bulk OUT ---- */
    7,                              /* bLength                             */
    TUSB_DESC_ENDPOINT,             /* bDescriptorType                     */
    EPNUM_GUD_OUT,                  /* bEndpointAddress                    */
    TUSB_XFER_BULK,                 /* bmAttributes                        */
    U16_TO_U8S_LE(GUD_EP_SIZE),     /* wMaxPacketSize                      */
    0,                              /* bInterval, ignored for bulk         */

    /* ---- Interface 1: the HID gamepad ----
     * interface number, string index, boot protocol, report descriptor
     * length, EP IN address, EP size, polling interval in ms. */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

    /* ---- Interface 2: the touch screen ----
     * bInterval 10 ms, the same as the gamepad. A resistive panel read with
     * 8 averaged samples cannot usefully go faster, and every interrupt
     * endpoint reserves bandwidth in every frame whether or not it is used. */
    TUD_HID_DESCRIPTOR(ITF_NUM_TOUCH, 6, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_touch_report), EPNUM_TOUCH_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_configuration;
}

/* ------------------------------------------------------------------ */
/* String descriptors                                                  */
/* ------------------------------------------------------------------ */

enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL,
       STRID_GUD_ITF, STRID_HID_ITF };

/*
 * These two strings decide what the gamepad is called in Linux, and the rule
 * is not obvious: usbhid_probe() in drivers/hid/usbhid/hid-core.c builds the
 * input device's name by concatenating the USB *device* manufacturer and
 * product strings with a space between them --
 *
 *     "FE Ljubljana" + " " + "MiSKo3 Display + Gamepad"
 *
 * -- and it ignores iInterface entirely, so the "MiSKo3 Gamepad" string
 * attached to the HID interface below never appears anywhere. Splitting the
 * wanted name across the two fields is the only way to get exactly
 * "MiSKo3 Gamepad" out of js0, /dev/input, and Steam.
 *
 * The display half is unaffected: KDE names that output "USB-1" from the DRM
 * connector, not from any USB string.
 */
static const char *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },   /* 0: English (0x0409), little endian */
    "MiSKo3",                        /* 1: Manufacturer -> "MiSKo3 ..."    */
    "Gamepad",                       /* 2: Product      -> "... Gamepad"   */
    NULL,                            /* 3: Serial, built at runtime        */
    "MiSKo3 GUD",                    /* 4: GUD interface (unused by hid)   */
    "MiSKo3 Gamepad",                /* 5: gamepad interface               */
    "MiSKo3 Touch",                  /* 6: touch screen interface          */
};

/*
 * Serial number from the STM32's 96-bit unique device ID.
 *
 * RM0440 puts it at 0x1FFF7590 in the system memory area, three read-only
 * 32-bit words programmed at the factory. Using it means two boards plugged
 * into the same machine get distinct device nodes instead of colliding, and it
 * costs nothing.
 */
#define UID_BASE_ADDR 0x1FFF7590u

static void build_serial(char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    const volatile uint32_t *uid = (const volatile uint32_t *) UID_BASE_ADDR;
    size_t pos = 0;

    for (int w = 0; w < 3; w++) {
        uint32_t v = uid[w];
        for (int nibble = 7; nibble >= 0; nibble--) {
            if (pos + 1 >= out_size)
                break;
            out[pos++] = hex[(v >> (nibble * 4)) & 0xFu];
        }
    }
    out[pos] = '\0';
}

/* Descriptor is built here on demand. 32 entries is 31 characters plus the
 * length/type word, which covers the 24-character serial. */
static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;

    size_t chr_count;
    char serial[25];
    const char *str;

    if (index == STRID_LANGID) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;

        if (index == STRID_SERIAL) {
            build_serial(serial, sizeof(serial));
            str = serial;
        } else {
            str = string_desc_arr[index];
        }

        chr_count = strlen(str);
        const size_t max_count = sizeof(desc_str) / sizeof(desc_str[0]) - 1;
        if (chr_count > max_count)
            chr_count = max_count;

        /* USB string descriptors are UTF-16LE; our source is plain ASCII. */
        for (size_t i = 0; i < chr_count; i++)
            desc_str[1 + i] = (uint16_t) str[i];
    }

    /* First word: length in bytes in the low byte, descriptor type in the
     * high byte. */
    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return desc_str;
}
