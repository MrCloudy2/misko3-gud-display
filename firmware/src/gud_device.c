/*
 * The GUD protocol state machine.
 *
 * The host's side of this conversation is drivers/gpu/drm/gud/ in the Linux
 * kernel. Reading gud_probe() there tells you exactly what has to work for the
 * driver to bind, and in what order:
 *
 *   1. It finds a bulk OUT endpoint on the interface. No endpoint, no bind.
 *   2. GUD_REQ_GET_DESCRIPTOR -- checks the magic, the version, and that the
 *      width/height ranges are sane. A wrong magic is reported as
 *      "Not a display interface" and probing stops.
 *   3. GUD_REQ_GET_FORMATS -- at least one format it understands.
 *   4. GUD_REQ_GET_PROPERTIES -- may legitimately be empty.
 *   5. GUD_REQ_GET_CONNECTORS -- at least one connector.
 *   6. Per connector: GET_CONNECTOR_PROPERTIES, GET_CONNECTOR_STATUS, then
 *      GET_CONNECTOR_EDID and GET_CONNECTOR_MODES when userspace probes.
 *
 * After that the display is registered with DRM, and the SET_* requests start
 * arriving as userspace configures and draws to it.
 */

#include <string.h>

#include "gud_device.h"
#include "rtt.h"

/* ------------------------------------------------------------------ */
/* What this display is                                                */
/* ------------------------------------------------------------------ */

/*
 * Pixel formats we accept, in the order the host should prefer them.
 *
 * RGB565 only, and only because it is what the panel natively wants: the
 * ILI9341 is configured for 16 bits per pixel, so RGB565 pixels go straight
 * from the USB buffer to the FMC with no conversion at all.
 *
 * Advertising just this one still gives userspace two formats. The kernel sees
 * that we do not support XRGB8888, picks RGB565 as the "emulation format", and
 * offers XRGB8888 to userspace anyway, converting on the host with
 * drm_fb_xrgb8888_to_rgb565(). That conversion is far cheaper on a desktop CPU
 * than anything we could do here, and it halves the bytes on the wire compared
 * to sending XRGB8888 -- which matters, because USB is this project's
 * bottleneck.
 */
static const uint8_t gud_formats[] = {
    GUD_PIXEL_FORMAT_RGB565,
};

/*
 * The pixel format currently committed by the host. Needed to validate
 * SET_BUFFER: the host tells us a rectangle and a byte count, and the two have
 * to agree. Starts as RGB565 because that is the only thing we accept.
 */
static uint8_t gud_active_format = GUD_PIXEL_FORMAT_RGB565;

/* The most recent SET_BUFFER. The bulk transfer that follows belongs to it. */
static struct gud_set_buffer_req gud_set_buf;

const struct gud_set_buffer_req *gud_current_buffer(void)
{
    return &gud_set_buf;
}

uint32_t gud_buffer_length(uint8_t format, uint32_t width, uint32_t height)
{
    if (!width || !height)
        return 0;

    switch (format) {
    case GUD_PIXEL_FORMAT_RGB565:
        return width * height * 2u;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* IN requests: the host asking us about ourselves                     */
/* ------------------------------------------------------------------ */

/*
 * Copy at most max_len bytes of a fixed-size answer. Short reads are legal
 * throughout this protocol -- the host caps wLength at whatever buffer it has
 * -- so truncating is correct behaviour, not an error.
 */
static int copy_out(uint8_t *out, uint16_t max_len, const void *src, uint32_t len)
{
    if (len > max_len)
        len = max_len;
    memcpy(out, src, len);
    return (int) len;
}

static int req_get_descriptor(uint8_t *out, uint16_t max_len)
{
    struct gud_display_descriptor_req desc;

    memset(&desc, 0, sizeof(desc));

    desc.magic = GUD_DISPLAY_MAGIC;
    desc.version = 1;

    /*
     * flags = 0.
     *
     * STATUS_ON_SET exists for the Linux USB *gadget* implementation, which
     * cannot control the status stage of a control OUT that carries data; we
     * can, so we do not need it. Without it the host only reads GUD_REQ_
     * GET_STATUS when a request actually failed, which is one fewer control
     * transfer per update.
     *
     * FULL_UPDATE means "always send me the whole framebuffer", which would
     * throw away damage tracking, and the kernel refuses it in combination
     * with compression anyway (gud_probe() returns -EINVAL).
     */
    desc.flags = 0;

    /*
     * LZ4, the only compression GUD defines.
     *
     * This is the single most valuable bit in the whole descriptor. M4
     * measured the uncompressed pipeline at 0.768 MB/s over USB against a
     * 3.18 ms blit -- USB is 98.4% of every frame and the FMC is 1.6%. Nothing
     * on the device side can improve that; only sending fewer bytes can.
     *
     * Setting this bit makes the host call LZ4_compress_default() on every
     * damage rectangle before sending it (gud_prep_flush() in gud_pipe.c) and
     * report both lengths in GUD_REQ_SET_BUFFER. If a rectangle does not
     * compress -- LZ4 returns 0 when the result would not fit in the original
     * size -- the host silently falls back to sending it raw, so the device
     * must handle both cases on every transfer.
     *
     * Note this could not be combined with GUD_DISPLAY_FLAG_FULL_UPDATE even
     * if we wanted it: gud_probe() returns -EINVAL for that combination.
     */
    desc.compression = GUD_COMPRESSION_LZ4;

    desc.max_buffer_size = GUD_MAX_BUFFER_SIZE;

    /* One fixed mode: min == max on both axes tells DRM this display has
     * exactly one size. */
    desc.min_width = GUD_WIDTH;
    desc.max_width = GUD_WIDTH;
    desc.min_height = GUD_HEIGHT;
    desc.max_height = GUD_HEIGHT;

    return copy_out(out, max_len, &desc, sizeof(desc));
}

static int req_get_formats(uint8_t *out, uint16_t max_len)
{
    return copy_out(out, max_len, gud_formats, sizeof(gud_formats));
}

/*
 * Properties are optional DRM knobs (rotation, backlight brightness, TV
 * overscan...). We expose none, so the answer is an empty array. The kernel
 * handles a zero-length reply: num_properties becomes 0 and it moves on.
 */
static int req_get_properties(uint8_t *out, uint16_t max_len)
{
    (void) out;
    (void) max_len;
    return 0;
}

static int req_get_connectors(uint8_t *out, uint16_t max_len)
{
    struct gud_connector_descriptor_req conn;

    if (max_len < sizeof(conn))
        return -GUD_STATUS_PROTOCOL_ERROR;

    memset(&conn, 0, sizeof(conn));

    /* PANEL, because that is what it is: a fixed LCD soldered to the board,
     * not a socket someone can unplug. */
    conn.connector_type = GUD_CONNECTOR_TYPE_PANEL;

    /*
     * No POLL_STATUS flag. That flag asks the host to poll our connector
     * status every 10 seconds, which is useful for a device whose display can
     * be unplugged, and useful as a keep-alive to notice a wedged link. Ours
     * cannot be unplugged, and every control transfer competes with pixels for
     * the same 12 Mbit/s, so we leave it off.
     */
    conn.flags = 0;

    return copy_out(out, max_len, &conn, sizeof(conn));
}

static int req_get_connector_properties(uint8_t *out, uint16_t max_len)
{
    (void) out;
    (void) max_len;
    return 0;
}

static int req_get_connector_status(uint8_t *out, uint16_t max_len)
{
    uint8_t status = GUD_CONNECTOR_STATUS_CONNECTED;

    if (max_len < sizeof(status))
        return -GUD_STATUS_PROTOCOL_ERROR;

    return copy_out(out, max_len, &status, sizeof(status));
}

/*
 * The single display mode.
 *
 * A DRM mode is normally a full CRT-style timing: active pixels, then front
 * porch, sync pulse and back porch on each axis. This panel has none of that
 * -- the ILI9341 has its own internal timing generator and we just write
 * pixels into its RAM over a parallel bus. So every porch is zero and htotal /
 * vtotal equal the visible size.
 *
 * The clock field is the pixel clock in kHz, and DRM derives the refresh rate
 * from it:
 *
 *      refresh = clock * 1000 / (htotal * vtotal)
 *
 * Rearranged for the rate we want:
 *
 *      clock = htotal * vtotal * refresh / 1000
 *            = 320 * 240 * 60 / 1000
 *            = 4608 kHz
 *
 * 60 Hz is a deliberate choice rather than the panel's true 80.95 Hz. This
 * number is what the compositor uses to pace its repaints, and USB holds us
 * well below 60 fps for anything but the flattest content, so asking the host
 * to prepare 81 frames a second would only waste its time. The panel's real
 * refresh still matters -- M6 syncs blits to it with GET_SCANLINE -- but that
 * is independent of what we advertise here.
 */
static int req_get_connector_modes(uint8_t *out, uint16_t max_len)
{
    struct gud_display_mode_req mode;

    if (max_len < sizeof(mode))
        return -GUD_STATUS_PROTOCOL_ERROR;

    memset(&mode, 0, sizeof(mode));

    mode.hdisplay = GUD_WIDTH;
    mode.hsync_start = GUD_WIDTH;
    mode.hsync_end = GUD_WIDTH;
    mode.htotal = GUD_WIDTH;

    mode.vdisplay = GUD_HEIGHT;
    mode.vsync_start = GUD_HEIGHT;
    mode.vsync_end = GUD_HEIGHT;
    mode.vtotal = GUD_HEIGHT;

    mode.clock = (GUD_WIDTH * GUD_HEIGHT * 60u) / 1000u;   /* 4608 kHz */
    mode.flags = GUD_DISPLAY_MODE_FLAG_PREFERRED;

    return copy_out(out, max_len, &mode, sizeof(mode));
}

/*
 * We have no EDID. Returning zero bytes is the documented way to say so:
 * "If GUD_REQ_GET_CONNECTOR_MODES returns zero, EDID is used to create display
 * modes" -- and the converse, which is our case, is that a zero-length EDID
 * makes the host fall back to the mode list above.
 *
 * A synthesised EDID would buy a nicer name in the display settings dialog and
 * a physical size in millimetres. Not worth 128 bytes of hand-built binary
 * with a checksum at this stage; it can be added in M7 if the KDE display
 * panel looks bare.
 */
static int req_get_connector_edid(uint8_t *out, uint16_t max_len)
{
    (void) out;
    (void) max_len;
    return 0;
}

int gud_handle_get(uint8_t request, uint16_t index, uint8_t *out, uint16_t max_len)
{
    /*
     * wValue carries the connector index for the connector requests and must
     * be zero for everything else. We have exactly one connector, so anything
     * but zero is a protocol error either way.
     */
    if (index != 0u)
        return -GUD_STATUS_PROTOCOL_ERROR;

    if (max_len == 0u)
        return -GUD_STATUS_PROTOCOL_ERROR;

    switch (request) {
    case GUD_REQ_GET_DESCRIPTOR:
        return req_get_descriptor(out, max_len);
    case GUD_REQ_GET_FORMATS:
        return req_get_formats(out, max_len);
    case GUD_REQ_GET_PROPERTIES:
        return req_get_properties(out, max_len);
    case GUD_REQ_GET_CONNECTORS:
        return req_get_connectors(out, max_len);
    case GUD_REQ_GET_CONNECTOR_PROPERTIES:
        return req_get_connector_properties(out, max_len);
    case GUD_REQ_GET_CONNECTOR_STATUS:
        return req_get_connector_status(out, max_len);
    case GUD_REQ_GET_CONNECTOR_MODES:
        return req_get_connector_modes(out, max_len);
    case GUD_REQ_GET_CONNECTOR_EDID:
        return req_get_connector_edid(out, max_len);
    default:
        return -GUD_STATUS_REQUEST_NOT_SUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* OUT requests: the host changing our state                           */
/* ------------------------------------------------------------------ */

/*
 * Validate the rectangle and the byte count that go with the bulk transfer we
 * are about to receive.
 *
 * These checks are the device's only defence. Everything that follows -- the
 * size of the bulk transfer, where the pixels get written on the panel -- is
 * derived from these numbers, so a bad one that gets through here becomes a
 * buffer overrun or a wild FMC write later.
 */
static int req_set_buffer(const struct gud_set_buffer_req *req, uint16_t len)
{
    uint32_t expected;

    if (len != sizeof(*req))
        return -GUD_STATUS_PROTOCOL_ERROR;

    /* Rectangle must lie inside the panel. Written as two comparisons per axis
     * so that x + width cannot wrap: x is already known to be below the width
     * when the sum is evaluated. */
    if (req->x >= GUD_WIDTH || req->y >= GUD_HEIGHT ||
        req->width > GUD_WIDTH - req->x ||
        req->height > GUD_HEIGHT - req->y)
        return -GUD_STATUS_INVALID_PARAMETER;

    /* The uncompressed size must be exactly what that rectangle needs. */
    expected = gud_buffer_length(gud_active_format, req->width, req->height);
    if (expected == 0u || req->length != expected)
        return -GUD_STATUS_INVALID_PARAMETER;

    /* We told the host our largest buffer; it must respect it. */
    if (req->length > GUD_MAX_BUFFER_SIZE)
        return -GUD_STATUS_INVALID_PARAMETER;

    /*
     * Compression. The host may send any given rectangle either way, deciding
     * per rectangle whether LZ4 actually helped, so both paths must be valid.
     *
     * The bounds matter. compressed_length is what will arrive on the bulk
     * endpoint and is used verbatim to size the transfer, so it has to be
     * inside the receive buffer. It also cannot exceed the uncompressed
     * length: the host compresses with dstCapacity == srcSize and gives up if
     * the result would be larger, so a compressed_length above length means
     * either a protocol error or a corrupted request.
     */
    if (req->compression & ~((uint8_t) GUD_COMPRESSION_LZ4))
        return -GUD_STATUS_INVALID_PARAMETER;     /* an algorithm we never offered */

    if (req->compression) {
        if (req->compressed_length == 0u ||
            req->compressed_length > req->length ||
            req->compressed_length > GUD_MAX_BUFFER_SIZE)
            return -GUD_STATUS_INVALID_PARAMETER;
    } else if (req->compressed_length != 0u) {
        return -GUD_STATUS_INVALID_PARAMETER;
    }

    gud_set_buf = *req;
    return 0;
}

/*
 * SET_STATE_CHECK is DRM's atomic check phase: "could you do this?". The
 * answer must not change anything the host can observe, because the commit
 * may never come. We only record the format so SET_BUFFER can validate
 * against it, and that is harmless -- nothing is drawn until COMMIT.
 */
static int req_set_state_check(const struct gud_state_req *req, uint16_t len)
{
    unsigned int i;

    if (len < sizeof(*req))
        return -GUD_STATUS_PROTOCOL_ERROR;

    /* Anything after the fixed part is an array of properties. We advertised
     * none, so a whole number of them is fine but a partial one is not. */
    if ((len - sizeof(*req)) % sizeof(struct gud_property_req))
        return -GUD_STATUS_PROTOCOL_ERROR;

    if (req->mode.hdisplay != GUD_WIDTH || req->mode.vdisplay != GUD_HEIGHT)
        return -GUD_STATUS_INVALID_PARAMETER;

    if (req->connector != 0u)
        return -GUD_STATUS_INVALID_PARAMETER;

    for (i = 0; i < sizeof(gud_formats); i++)
        if (req->format == gud_formats[i])
            break;
    if (i == sizeof(gud_formats))
        return -GUD_STATUS_INVALID_PARAMETER;

    gud_active_format = req->format;

    rtt_printf("[gud] state check: %ux%u format 0x%02X\r\n",
               (unsigned) req->mode.hdisplay, (unsigned) req->mode.vdisplay,
               (unsigned) req->format);
    return 0;
}

static int req_set_controller_enable(const uint8_t *enable, uint16_t len)
{
    if (len != 1u)
        return -GUD_STATUS_PROTOCOL_ERROR;

    rtt_printf("[gud] controller %s\r\n", *enable ? "ENABLE" : "disable");
    gud_panel_controller_enable(*enable);
    return 0;
}

static int req_set_display_enable(const uint8_t *enable, uint16_t len)
{
    if (len != 1u)
        return -GUD_STATUS_PROTOCOL_ERROR;

    rtt_printf("[gud] display %s\r\n", *enable ? "ON" : "off");
    gud_panel_display_enable(*enable);
    return 0;
}

int gud_handle_set(uint8_t request, uint16_t index, const uint8_t *in, uint16_t len)
{
    if (index != 0u)
        return -GUD_STATUS_PROTOCOL_ERROR;

    switch (request) {
    case GUD_REQ_SET_CONNECTOR_FORCE_DETECT:
        /* "Userspace is about to ask whether you are connected." Nothing to
         * do: this panel is soldered on and always connected. */
        return 0;

    case GUD_REQ_SET_BUFFER:
        return req_set_buffer((const struct gud_set_buffer_req *) in, len);

    case GUD_REQ_SET_STATE_CHECK:
        return req_set_state_check((const struct gud_state_req *) in, len);

    case GUD_REQ_SET_STATE_COMMIT:
        rtt_printf("[gud] state commit\r\n");
        return gud_panel_state_commit(NULL, gud_active_format);

    case GUD_REQ_SET_CONTROLLER_ENABLE:
        return req_set_controller_enable(in, len);

    case GUD_REQ_SET_DISPLAY_ENABLE:
        return req_set_display_enable(in, len);

    default:
        return -GUD_STATUS_REQUEST_NOT_SUPPORTED;
    }
}
