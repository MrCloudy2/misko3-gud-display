/*
 * Minimal SEGGER RTT up-channel (target -> host).
 *
 * Why this exists: probe-rs finds RTT by scanning target RAM for the 16-byte
 * ID string "SEGGER RTT". Once it finds that control block it knows where the
 * ring buffer is, and it drains it over SWD while the core keeps running.
 * That gives us a console with zero extra wiring -- no UART pins (all the
 * convenient ones are eaten by the FMC bus) and no SWO setup.
 *
 * We only implement the up (target->host) direction; we never need host->target.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "rtt.h"

#define RTT_BUFFER_SIZE 4096

/* Flags field of a buffer descriptor. 2 = block if full, i.e. the target spins
 * until the host has drained enough room. We want that for benchmark output:
 * silently dropping a line would corrupt the results we are trying to report.
 * The spin is bounded below so a detached host cannot wedge the board. */
#define RTT_MODE_BLOCK_IF_FULL 2

typedef struct {
    const char *sName;
    char *pBuffer;
    uint32_t SizeOfBuffer;
    uint32_t WrOff;          /* target writes here  */
    volatile uint32_t RdOff; /* host advances this  */
    uint32_t Flags;
} rtt_buffer_up_t;

typedef struct {
    const char *sName;
    char *pBuffer;
    uint32_t SizeOfBuffer;
    volatile uint32_t WrOff;
    uint32_t RdOff;
    uint32_t Flags;
} rtt_buffer_down_t;

typedef struct {
    char acID[16];
    int32_t MaxNumUpBuffers;
    int32_t MaxNumDownBuffers;
    rtt_buffer_up_t aUp[1];
    rtt_buffer_down_t aDown[1];
} rtt_cb_t;

static char rtt_up_buffer[RTT_BUFFER_SIZE];
static char rtt_down_buffer[16];

/* Must live in RAM: the host both reads and writes fields of this block. */
rtt_cb_t _SEGGER_RTT;

void rtt_init(void)
{
    _SEGGER_RTT.MaxNumUpBuffers = 1;
    _SEGGER_RTT.MaxNumDownBuffers = 1;

    _SEGGER_RTT.aUp[0].sName = "Terminal";
    _SEGGER_RTT.aUp[0].pBuffer = rtt_up_buffer;
    _SEGGER_RTT.aUp[0].SizeOfBuffer = sizeof(rtt_up_buffer);
    _SEGGER_RTT.aUp[0].WrOff = 0;
    _SEGGER_RTT.aUp[0].RdOff = 0;
    _SEGGER_RTT.aUp[0].Flags = RTT_MODE_BLOCK_IF_FULL;

    _SEGGER_RTT.aDown[0].sName = "Terminal";
    _SEGGER_RTT.aDown[0].pBuffer = rtt_down_buffer;
    _SEGGER_RTT.aDown[0].SizeOfBuffer = sizeof(rtt_down_buffer);
    _SEGGER_RTT.aDown[0].WrOff = 0;
    _SEGGER_RTT.aDown[0].RdOff = 0;
    _SEGGER_RTT.aDown[0].Flags = 0;

    /* Write the magic LAST. Until these bytes land, a host scanning RAM will
     * not recognise the block, so it can never latch onto a half-built one. */
    const char id[16] = {'S','E','G','G','E','R',' ','R','T','T',0,0,0,0,0,0};
    for (int i = 0; i < 16; i++)
        _SEGGER_RTT.acID[i] = id[i];
}

/*
 * Write one character, NEVER blocking.
 *
 * This function used to wait for the host to make room, on the theory that
 * dropping a line of benchmark output would corrupt a measurement. On a device
 * that is also a display and a gamepad that trade is simply wrong, and it
 * caused a hard hang that looked exactly like a crash:
 *
 *   The earlier compromise blocked only while RdOff was non-zero, taking that
 *   as proof a host was listening. But when probe-rs *detaches*, RdOff keeps
 *   whatever value it last wrote -- so the firmware went on believing a host
 *   was there. The buffer filled, and every subsequent character burned the
 *   full million-iteration guard: about 23 ms each, seconds per line. tud_task()
 *   stopped being called, USB starved, and the panel froze with the gamepad
 *   dead while the CPU sat in this loop in Thread mode with no fault set.
 *
 *   Adding the analogue stick turned that from rare into routine, because
 *   reports (and log lines) went from "only when a button changes" to up to
 *   100 per second of stick noise.
 *
 * So: if there is no room, the character is dropped and we move on. Losing
 * console output is a nuisance; stalling the USB stack breaks the product.
 */
static void rtt_write_char(char c)
{
    uint32_t wr = _SEGGER_RTT.aUp[0].WrOff;
    uint32_t next = wr + 1;

    if (next >= RTT_BUFFER_SIZE)
        next = 0;

    if (next == _SEGGER_RTT.aUp[0].RdOff)
        return;                 /* full: drop, never wait */

    rtt_up_buffer[wr] = c;
    _SEGGER_RTT.aUp[0].WrOff = next;
}

void rtt_write(const char *s)
{
    while (*s)
        rtt_write_char(*s++);
}

void rtt_printf(const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    rtt_write(line);
}
