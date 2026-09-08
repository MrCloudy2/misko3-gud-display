/*
 * TinyUSB glue for the GUD display: a custom USB device class driver.
 *
 * Why a custom class driver rather than TinyUSB's built-in vendor class:
 *
 * The built-in class copies bulk data through its own FIFO, which would mean a
 * third 40 KB buffer we cannot afford, and it gives no way to say "the next
 * bulk transfer is exactly N bytes and belongs here". GUD needs precisely
 * that: a SET_BUFFER control request announces the size and destination of the
 * bulk transfer that immediately follows. So we register our own driver
 * through usbd_app_driver_get_cb() and drive the endpoint directly. This is
 * the same approach gud-pico takes.
 *
 * Two separate paths meet in this file:
 *
 *   Control -- TinyUSB routes *every* vendor-type control request to
 *   tud_vendor_control_xfer_cb() (see process_setup_received() in usbd.c), no
 *   matter which interface it names. That is the hook we override. The class
 *   driver's own control_xfer_cb is therefore never used and returns false.
 *
 *   Bulk -- the class driver's open() claims the interface and opens the bulk
 *   OUT endpoint; xfer_cb() is called when a transfer we armed has landed.
 */

#include <string.h>

#include "stm32g4xx.h"      /* for DWT->CYCCNT, to time decompression */
#include "tusb.h"
#include "device/usbd_pvt.h"

#include "gud_device.h"
#include "lz4_dec.h"
#include "rtt.h"

/*
 * Buffer for control request payloads, in both directions.
 *
 * Everything this protocol carries over endpoint 0 is small: the display
 * descriptor is 30 bytes, a state request 26, a set-buffer request 25. 128
 * gives comfortable headroom and would also fit a 128-byte EDID block if we
 * ever synthesise one.
 *
 * The host frequently asks for more than we have -- GET_CONNECTOR_MODES is
 * requested with wLength = 3072, room for 128 modes -- so wLength is clamped
 * to this size and the answer comes back short. Short IN transfers are normal
 * in this protocol, not an error.
 */
#define GUD_CTRL_BUF_SIZE 128
static uint8_t gud_ctrl_buf[GUD_CTRL_BUF_SIZE];

/*
 * The band buffer. One buffer, not two -- this is the project's whole memory
 * architecture, and getting it down to one is what pays for the larger bands.
 *
 * Layout during a compressed transfer, for a rectangle of `length` pixels
 * arriving as `clen` compressed bytes:
 *
 *      gud_band_buf
 *      |<---------------- length ---------------->|<- margin ->|
 *      +------------------------------------------+------------+
 *      |         decompressed pixels              |            |
 *      +------------------------------------------+------------+
 *                                     |<--- clen --->|
 *                                     ^
 *                                     bulk data lands here
 *
 * The compressed block is received flush against the far end and decoded
 * forwards into the front of the same buffer. Both pointers move in the same
 * direction and the write pointer provably never catches the read pointer --
 * the reasoning and its host-side verification are in gud_device.h next to
 * GUD_INPLACE_MARGIN.
 *
 * An uncompressed transfer lands at offset 0 and is blitted directly, so it
 * never pays for a copy it does not need.
 *
 * Why the whole band has to exist in RAM at all: an LZ4 match can reach up to
 * 65,535 bytes backwards into output already produced, so the decoder must be
 * able to re-read what it wrote. It cannot be streamed to the FMC a few bytes
 * at a time.
 */
static uint8_t gud_band_buf[GUD_MAX_BUFFER_SIZE + GUD_INPLACE_MARGIN];

/* Where the compressed bytes of the transfer in flight were placed, so the
 * completion callback knows where to decode from. */
static uint8_t *gud_comp_at;

/*
 * Result of the most recent request, readable by the host with
 * GUD_REQ_GET_STATUS.
 *
 * The kernel only asks for this after a control transfer failed (it sees the
 * stall as -EPIPE), and uses it to turn our failure into a meaningful errno.
 * Without it every failure would look like a generic pipe error.
 */
static uint8_t gud_status = GUD_STATUS_OK;

/* State of the interface, filled in by open(). The endpoint descriptor is kept
 * because recovering a wedged endpoint means closing and reopening it, and
 * usbd_edpt_open() wants the descriptor again. */
static uint8_t gud_itf_num;
static uint8_t gud_ep_out;
static tusb_desc_endpoint_t gud_ep_desc;

/* How many bytes the currently armed bulk transfer expects. */
static uint32_t gud_bulk_len;

/* Counters, purely so the console can show that traffic is flowing. */
uint32_t gud_stat_buffers;
uint64_t gud_stat_bulk_bytes;    /* bytes actually on the wire        */
uint64_t gud_stat_raw_bytes;     /* bytes after decompression         */
uint64_t gud_stat_decomp_cycles; /* DWT cycles spent decompressing    */
uint32_t gud_stat_decomp_errors;
uint32_t gud_stat_ep_recoveries; /* stale bulk transfers discarded */
uint32_t gud_stat_errors;

/* ------------------------------------------------------------------ */
/* Control transfers                                                   */
/* ------------------------------------------------------------------ */

/*
 * SETUP stage. The host has sent an 8-byte request and we must decide what
 * happens next.
 *
 * Returning false stalls endpoint 0, which the kernel reads as -EPIPE and
 * follows with GUD_REQ_GET_STATUS to find out why.
 */
static bool gud_control_setup(uint8_t rhport, tusb_control_request_t const *req)
{
    uint16_t len = req->wLength;

    if (len > GUD_CTRL_BUF_SIZE)
        len = GUD_CTRL_BUF_SIZE;

    /*
     * Only vendor requests addressed to an interface are ours. TinyUSB sends
     * us every vendor request regardless of recipient, so this filter is what
     * stops us answering something that was meant for somebody else.
     */
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE)
        return false;

    if (req->bmRequestType_bit.direction == TUSB_DIR_IN) {
        int ret;

        /*
         * GET_STATUS is special: it reports the outcome of the *previous*
         * request, so it must not clear the status it is about to report.
         */
        if (req->bRequest == GUD_REQ_GET_STATUS)
            return tud_control_xfer(rhport, req, &gud_status, sizeof(gud_status));

        gud_status = GUD_STATUS_OK;

        ret = gud_handle_get((uint8_t) req->bRequest, req->wValue,
                             gud_ctrl_buf, len);
        if (ret < 0) {
            gud_status = (uint8_t) (-ret);
            gud_stat_errors++;
            return false;
        }

        return tud_control_xfer(rhport, req, gud_ctrl_buf, (uint16_t) ret);
    }

    /* OUT direction. */
    gud_status = GUD_STATUS_OK;

    /*
     * A request with no payload (SET_STATE_COMMIT, SET_CONNECTOR_FORCE_DETECT)
     * has no data stage, so CONTROL_STAGE_DATA will never fire for it. It has
     * to be acted on here instead.
     */
    if (len == 0u) {
        int ret = gud_handle_set((uint8_t) req->bRequest, req->wValue, NULL, 0);
        if (ret < 0) {
            gud_status = (uint8_t) (-ret);
            gud_stat_errors++;
            return false;
        }
    }

    /* Accept the payload (or send the status packet when there is none). */
    return tud_control_xfer(rhport, req, gud_ctrl_buf, len);
}

/*
 * DATA stage complete: the payload of an OUT request is now in gud_ctrl_buf.
 */
static bool gud_control_data(uint8_t rhport, tusb_control_request_t const *req)
{
    uint16_t len = req->wLength;
    int ret;

    if (len > GUD_CTRL_BUF_SIZE)
        len = GUD_CTRL_BUF_SIZE;

    /* IN requests have nothing left to do once their data has been sent. */
    if (req->bmRequestType_bit.direction == TUSB_DIR_IN)
        return true;

    /* Already handled at SETUP: no payload means no data stage. */
    if (len == 0u)
        return true;

    ret = gud_handle_set((uint8_t) req->bRequest, req->wValue, gud_ctrl_buf, len);
    if (ret < 0) {
        gud_status = (uint8_t) (-ret);
        gud_stat_errors++;
        return false;
    }

    /*
     * SET_BUFFER is the one request with a consequence beyond endpoint 0: the
     * host is about to push pixels at the bulk endpoint, and it will keep
     * pushing for three seconds before giving up (gud_usb_bulk() arms a
     * timeout in gud_pipe.c). So the receive has to be armed now, from inside
     * this callback, before the status stage completes.
     */
    if (req->bRequest == GUD_REQ_SET_BUFFER) {
        const struct gud_set_buffer_req *buf_req = gud_current_buffer();
        uint8_t *dest;

        /*
         * Where does the incoming data go?
         *
         * Compressed: flush against the far end of the buffer, at
         * length + margin - clen, so that decoding forwards into offset 0
         * finishes exactly as the read pointer reaches the end. Uncompressed:
         * straight to offset 0, ready to blit with no copy at all.
         *
         * The host chooses per rectangle -- it falls back to raw whenever LZ4
         * fails to shrink that particular band -- so this is decided fresh
         * every transfer.
         */
        if (buf_req->compression) {
            gud_bulk_len = buf_req->compressed_length;
        } else {
            gud_bulk_len = buf_req->length;
        }

        /* gud_handle_set() has already checked this against
         * GUD_MAX_BUFFER_SIZE; re-check because the consequence of being wrong
         * is a 75 KB overrun of a static buffer. */
        if (gud_bulk_len == 0u || gud_bulk_len > GUD_MAX_BUFFER_SIZE ||
            buf_req->length > GUD_MAX_BUFFER_SIZE) {
            gud_status = GUD_STATUS_INVALID_PARAMETER;
            gud_stat_errors++;
            return false;
        }

        if (buf_req->compression) {
            uint32_t margin = (gud_bulk_len >> 8) + 32u;
            gud_comp_at = gud_band_buf + buf_req->length + margin - gud_bulk_len;
            dest = gud_comp_at;
        } else {
            gud_comp_at = NULL;
            dest = gud_band_buf;
        }

        /*
         * Recover an endpoint left armed by a transfer that never arrived.
         *
         * The host does not always follow a SET_BUFFER with the bulk data it
         * promised. If a flush fails or is cancelled -- gud_usb_bulk() in
         * gud_pipe.c gives up after three seconds, and gud_flush_rect() then
         * re-sends SET_BUFFER on the next attempt because prev_flush_failed is
         * set -- we can be asked to arm a transfer while the previous one is
         * still outstanding. TinyUSB's usbd_edpt_xfer() asserts on that
         * ("Attempt to transfer on a busy endpoint, sound like a race
         * condition!"), which returns false normally and halts the CPU on a
         * BKPT when a debugger happens to be attached.
         *
         * The recovery is usbd_edpt_clear_stall(), which is not as odd as it
         * looks. It clears STALLED and BUSY unconditionally, and TinyUSB's own
         * comment on it says so: "some classes, e.g. audio's set-interface,
         * call this on a non-stalled endpoint solely to drop a leftover BUSY
         * bit". Dropping a leftover BUSY bit is exactly what is needed. It also
         * resets the endpoint's data toggle, which is what we want before a
         * fresh transfer anyway.
         *
         * The obvious-looking alternative, closing and reopening the endpoint,
         * does NOT work on this port and was tried first. usbd_edpt_close() is
         * compiled out to a pair of (void) casts whenever TUP_DCD_EDPT_ISO_ALLOC
         * is defined, which tusb_mcu.h does for every controller that lacks
         * TUP_DCD_EDPT_CLOSE_API -- STM32 fsdev among them. The call ran, did
         * nothing, and the assert fired anyway.
         *
         * Discarding the abandoned transfer is the right answer regardless: its
         * data belongs to a rectangle the host has already given up on.
         */
        if (usbd_edpt_busy(rhport, gud_ep_out)) {
            usbd_edpt_clear_stall(rhport, gud_ep_out);
            gud_stat_ep_recoveries++;
        }

        if (!usbd_edpt_xfer(rhport, gud_ep_out, dest,
                            (uint16_t) gud_bulk_len, false)) {
            gud_status = GUD_STATUS_ERROR;
            gud_stat_errors++;
            return false;
        }
    }

    return true;
}

/*
 * TinyUSB hands every vendor-type control request to this function, at each
 * stage of the transfer.
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *req)
{
    switch (stage) {
    case CONTROL_STAGE_SETUP:
        return gud_control_setup(rhport, req);
    case CONTROL_STAGE_DATA:
        return gud_control_data(rhport, req);
    default:
        /* CONTROL_STAGE_ACK: nothing to do. */
        return true;
    }
}

/* ------------------------------------------------------------------ */
/* Class driver                                                        */
/* ------------------------------------------------------------------ */

static void gud_drv_init(void)
{
    gud_itf_num = 0;
    gud_ep_out = 0;
    gud_bulk_len = 0;
}

static bool gud_drv_deinit(void)
{
    return true;
}

static void gud_drv_reset(uint8_t rhport)
{
    (void) rhport;
    gud_ep_out = 0;
    gud_bulk_len = 0;
}

/*
 * Called once per interface that no earlier driver has claimed. We take
 * vendor-specific interfaces and leave everything else alone -- which is what
 * lets the HID gamepad interface fall through to TinyUSB's own HID driver in
 * M3.
 *
 * The return value is how many bytes of the configuration descriptor we
 * consumed; returning 0 means "not mine".
 */
static uint16_t gud_drv_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc,
                             uint16_t max_len)
{
    uint16_t drv_len;
    tusb_desc_endpoint_t const *ep_desc;

    TU_VERIFY(itf_desc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC, 0);

    /* One interface descriptor followed by exactly one endpoint descriptor. */
    drv_len = (uint16_t) (sizeof(tusb_desc_interface_t)
                          + itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_VERIFY(max_len >= drv_len, 0);
    TU_VERIFY(itf_desc->bNumEndpoints == 1, 0);

    ep_desc = (tusb_desc_endpoint_t const *) tu_desc_next(itf_desc);
    TU_VERIFY(ep_desc->bDescriptorType == TUSB_DESC_ENDPOINT, 0);
    TU_VERIFY(ep_desc->bmAttributes.xfer == TUSB_XFER_BULK, 0);
    TU_VERIFY(tu_edpt_dir(ep_desc->bEndpointAddress) == TUSB_DIR_OUT, 0);

    TU_ASSERT(usbd_edpt_open(rhport, ep_desc), 0);

    gud_itf_num = itf_desc->bInterfaceNumber;
    gud_ep_out = ep_desc->bEndpointAddress;
    gud_ep_desc = *ep_desc;      /* kept so the endpoint can be reopened */

    return drv_len;
}

/* Control requests never reach the class driver: vendor requests are
 * intercepted by tud_vendor_control_xfer_cb() before driver dispatch. */
static bool gud_drv_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                    tusb_control_request_t const *req)
{
    (void) rhport;
    (void) stage;
    (void) req;
    return false;
}

/*
 * A bulk transfer we armed has completed. The pixels for the rectangle named
 * by the last SET_BUFFER are now in gud_band_buf.
 */
static bool gud_drv_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                            uint32_t xferred_bytes)
{
    (void) rhport;

    if (ep_addr != gud_ep_out)
        return false;

    if (result != XFER_RESULT_SUCCESS) {
        gud_stat_errors++;
        return false;
    }

    /*
     * A short transfer means the host sent fewer bytes than SET_BUFFER
     * promised. Rather than blit a partly-filled buffer, drop the frame: the
     * host will notice the failure and resend.
     */
    if (xferred_bytes != gud_bulk_len) {
        gud_stat_errors++;
        return false;
    }

    gud_stat_buffers++;
    gud_stat_bulk_bytes += xferred_bytes;

    /*
     * Decompress, if the host compressed it.
     *
     * The decoder is the one measured in M1: 380 bytes of code, 32 bytes of
     * stack, no static state, 21-34 MB/s of output on this part, and
     * bounds-checked on every read and write. That last property is what makes
     * it safe to run on data that arrived over a wire: a malformed block can
     * only produce an error, never a write outside gud_band_buf.
     *
     * The expected output size is known exactly -- the host told us in
     * GUD_REQ_SET_BUFFER -- so a block that decodes to any other size is
     * treated as a failure and the rectangle is dropped. The host notices and
     * resends rather than leaving a corrupt stripe on the panel.
     */
    const struct gud_set_buffer_req *buf_req = gud_current_buffer();

    if (buf_req->compression & GUD_COMPRESSION_LZ4) {
        uint32_t t0 = DWT->CYCCNT;

        /*
         * In place: source is inside the destination buffer, near its end.
         * The output capacity is exactly buf_req->length, not the whole
         * buffer -- that keeps the decoder's bounds check from ever letting it
         * write into the compressed bytes it has not read yet.
         */
        int got = lz4_decompress_block(gud_comp_at, gud_bulk_len,
                                       gud_band_buf, buf_req->length);

        gud_stat_decomp_cycles += (uint64_t) (DWT->CYCCNT - t0);

        if (got != (int) buf_req->length) {
            gud_stat_decomp_errors++;
            gud_stat_errors++;
            return false;
        }
    }

    gud_stat_raw_bytes += buf_req->length;

    gud_panel_write_buffer(buf_req, gud_band_buf);

    return true;
}

static const usbd_class_driver_t gud_driver[] = {
    {
        .name            = "GUD",
        .init            = gud_drv_init,
        .deinit          = gud_drv_deinit,
        .reset           = gud_drv_reset,
        .open            = gud_drv_open,
        .control_xfer_cb = gud_drv_control_xfer_cb,
        .xfer_cb         = gud_drv_xfer_cb,
        .xfer_isr        = NULL,
        .sof             = NULL,
    },
};

/*
 * TinyUSB calls this during tusb_init() to collect application class drivers.
 * They are tried before the built-in ones, which is why our open() has to
 * decline interfaces that are not vendor-specific.
 */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count += 1;
    return gud_driver;
}
