/*
 * Minimal LZ4 *block* decompressor.
 *
 * Scope, and why it is this narrow:
 *
 * LZ4 has two layers. The "frame" format (magic number 0x184D2204, block
 * checksums, content size, .lz4 files on disk) wraps the "block" format, which
 * is the actual compression. GUD uses the block format raw, with no frame:
 * the kernel's gud driver calls LZ4_compress_default() straight into the bulk
 * buffer (drivers/gpu/drm/gud/gud_pipe.c) and tells us both the compressed and
 * the uncompressed length in the GUD_REQ_SET_BUFFER request. So we need
 * exactly one function: decode a raw block whose output size we already know.
 *
 * This is deliberately a "safe" decoder in the LZ4 sense: every read from the
 * input and every write to the output is bounds-checked, so corrupt or hostile
 * input can return an error but can never run off either buffer. That costs a
 * few percent of speed and buys us not corrupting RAM on a device with no MMU.
 */

#ifndef LZ4_DEC_H
#define LZ4_DEC_H

#include <stdint.h>

/*
 * Decompress one LZ4 block.
 *
 *   src / src_len   the compressed block (no frame header)
 *   dst / dst_cap   where to put the result, and how much room there is
 *
 * Returns the number of bytes written, or a negative LZ4_DEC_E_* code if the
 * input is malformed. A correct block never writes more than dst_cap bytes.
 */
int lz4_decompress_block(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap);

#define LZ4_DEC_E_INPUT   (-1)   /* ran out of input, or a bad length code   */
#define LZ4_DEC_E_OUTPUT  (-2)   /* the result would not fit in dst_cap      */
#define LZ4_DEC_E_OFFSET  (-3)   /* match points outside the output produced */

#endif /* LZ4_DEC_H */
