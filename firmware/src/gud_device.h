/*
 * The GUD protocol state machine: what this display answers, and what it does
 * when the host sets something.
 *
 * Deliberately free of any USB code. Everything here is "given a request
 * number and a buffer, produce an answer" -- the TinyUSB plumbing lives in
 * gud_usbd.c, and the panel lives in panel.c. Keeping the three apart means
 * the protocol can be reasoned about (and explained) on its own.
 */

#ifndef GUD_DEVICE_H
#define GUD_DEVICE_H

#include <stdint.h>
#include "gud.h"

/* The panel. 320x240 landscape, the only mode this display has. */
#define GUD_WIDTH  320u
#define GUD_HEIGHT 240u

/*
 * The largest buffer we can be sent in one go, declared in the display
 * descriptor. The host divides every damage rectangle into horizontal bands of
 * (max_buffer_size / bytes-per-line) lines and sends each band as its own
 * SET_BUFFER + bulk transfer -- see gud_flush_damage() in gud_pipe.c.
 *
 * 76800 bytes is 120 lines of 320 RGB565 pixels: exactly half a frame, so a
 * full-screen update arrives as TWO rectangles instead of the four that M5's
 * 64-line bands produced. Both halves of that matter, and both were measured:
 *
 *   Fewer rectangles. M5 found that on highly compressible content the limit
 *   is neither USB nor the CPU but the GUD_REQ_SET_BUFFER control transfer
 *   that must precede every single rectangle -- at 29 fps, roughly 4 ms of
 *   each rectangle's 8.6 ms went to protocol overhead. Halving the rectangle
 *   count halves that.
 *
 *   Better compression. Matches cannot cross a band boundary, so fewer, larger
 *   bands compress better. Measured on the M1 corpus, going from four bands to
 *   two lifts flat UI content from 26.8x to 38.2x and terminal text from 9.3x
 *   to 11.4x -- the band-splitting penalty drops from +46%/+33% to +2.7%/+9.2%
 *   of the whole-frame ideal.
 *
 * Why not larger: 120 lines is already the smallest size that yields two
 * rectangles for a 240-line frame, so anything bigger costs RAM without
 * reducing the rectangle count further. One frame in a single rectangle would
 * need 153600 bytes and this part has 131072.
 *
 * Why this now fits at all: M5 needed two buffers of this size, one for the
 * compressed bytes and one for the decompressed pixels. This build decompresses
 * in place, so it needs one -- see GUD_INPLACE_MARGIN.
 */
#define GUD_MAX_BUFFER_SIZE 76800u

/*
 * Slack that makes in-place decompression safe.
 *
 * The compressed block is placed at the END of the output buffer and decoded
 * forwards into the front of it, so both pointers travel in the same direction
 * and the write pointer must never overtake the read pointer.
 *
 * Why it cannot: per LZ4 sequence the input advances by 1 (token) + 2 (offset)
 * + literals + any extended-length bytes, while the output advances by
 * literals + match, and a match is at least 4 bytes. Output therefore outruns
 * input by at least one byte per sequence, monotonically, so the gap is
 * narrowest at the very end -- where it is exactly (decompressed - compressed)
 * bytes. Starting the read pointer that far ahead, plus this margin, keeps it
 * ahead throughout.
 *
 * The margin is upstream LZ4's formula, (compressedSize >> 8) + 32. Strictly
 * this decoder needs less, because it copies exactly the number of bytes it
 * was asked for; the reference decoder overshoots by up to 32 bytes with its
 * "wildcopy" and that is what the 32 is for. Using the standard figure costs
 * 332 bytes and means the buffer is still correctly sized if this is ever
 * swapped for the reference implementation.
 *
 * Verified on the host: 20,000 in-place blocks round-tripped byte-exact under
 * AddressSanitizer with guard bands, 5,790 of them barely compressible, which
 * is the case where the two pointers come closest. See m1_lz4/host/fuzz_lz4.c.
 */
#define GUD_INPLACE_MARGIN ((GUD_MAX_BUFFER_SIZE >> 8) + 32u)

/*
 * Handle a vendor IN request (host is asking us something).
 *
 * Writes at most max_len bytes to out. Returns the number of bytes written, or
 * the negation of a GUD_STATUS_* code on failure -- the caller turns that into
 * a stalled control transfer plus a status byte the host can read back with
 * GUD_REQ_GET_STATUS.
 */
int gud_handle_get(uint8_t request, uint16_t index, uint8_t *out, uint16_t max_len);

/*
 * Handle a vendor OUT request (host is telling us something).
 * Returns 0 on success or the negation of a GUD_STATUS_* code.
 */
int gud_handle_set(uint8_t request, uint16_t index, const uint8_t *in, uint16_t len);

/* The rectangle described by the most recent GUD_REQ_SET_BUFFER. The bulk
 * transfer that follows belongs to it. */
const struct gud_set_buffer_req *gud_current_buffer(void);

/* Bytes one rectangle of this pixel format occupies, or 0 if we do not know
 * the format. */
uint32_t gud_buffer_length(uint8_t format, uint32_t width, uint32_t height);

/* ------------------------------------------------------------------ */
/* Implemented by the panel side (panel.c)                             */
/* ------------------------------------------------------------------ */

/* DRM is enabling or disabling the CRTC. */
void gud_panel_controller_enable(uint8_t enable);

/* DPMS: turn the actual output (here, the backlight) on or off. */
void gud_panel_display_enable(uint8_t enable);

/* A modeset has been committed. Returns 0, or -GUD_STATUS_* to refuse. */
int gud_panel_state_commit(const struct gud_state_req *state, uint8_t format);

/* One rectangle of pixels has arrived and been decompressed. */
void gud_panel_write_buffer(const struct gud_set_buffer_req *req,
                            const uint8_t *pixels);

#endif /* GUD_DEVICE_H */
