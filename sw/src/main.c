/*
 * ARM side of ZYNQ-FORTH (bare metal, Cortex-A9 core 0) on the USB UART, 115200 8N1.
 *
 * Loads the Forth kernel image into the HF16 RAM over AXI-Lite, verifies it and releases
 * the CPU. The UART then carries Forth's terminal as plain bytes, so any serial terminal
 * talks to Forth directly. The ZYNQ-FORTH app additionally sends service requests: a
 * request starts with SOH (0x01) and ends with LF; the answer starts with SOH as well and
 * is never mixed into Forth's output. A 0x01 that Forth itself prints goes out doubled.
 * Ctrl-R (0x12) typed at a terminal restarts Forth. The protocol: docs/Protocol.md.
 *
 * The input ring (64 KB, a whole source fits) feeds the RX FIFO as Forth reads it. Forth's
 * output waits in the TX FIFO when the UART is busy, so nothing is ever dropped.
 *
 * One Forth source can be saved in the QSPI flash (flash.c). At power-up and at every
 * restart it is fed to Forth after the kernel (autostart), unless autostart is off,
 * BTN3 is held, or the restart asks to skip it.
 *
 * The dictionary and the data stack are read while the CPU is paused (CTRL bit 1); the
 * stack is the copy the kernel's QUIT makes before each line. Register map:
 * docs/design.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xil_io.h"
#include "xparameters.h"
#include "xtime_l.h"
#include "xuartps_hw.h"
#include "flash.h"
#include "forth_image.h"

#define FIRMWARE_VERSION    "1.0"
#define PROTOCOL_VERSION    1

#define UART                STDIN_BASEADDRESS   /* PS UART0, 115200 8N1 from the BSP */

#define FORTH_BASE          0x43C00000u     /* fixed in hw/build_hw.tcl */
#define REG_CTRL            0x00
#define REG_MEM_ADDR        0x04
#define REG_MEM_DATA        0x08
#define REG_TX_POP          0x0C
#define REG_RX_PUSH         0x10
#define REG_STATUS          0x14
#define REG_BOARD           0x18
#define REG_PORT0           0x1C            /* three ports, 4 bytes apart */
#define REG_PWM             0x28
#define REG_XADC            0x2C

#define CTRL_HOLD           0x1
#define CTRL_PAUSE          0x2
#define STATUS_RX_FULL      0x1
#define STATUS_BTN3         0x80            /* debounced, STATUS bits 7..4 = BTN3..BTN0 */
#define TX_VALID            0x100

#define SOH                 0x01
#define CTRL_R              0x12            /* restart from a terminal program */
#define LINE_MAX            128
#define REQUEST_TIMEOUT     (COUNTS_PER_SECOND * 2)     /* between two bytes of a request */

#define IN_SIZE             65536u          /* powers of two */
#define OUT_SIZE            4096u

#define SOURCE_MAGIC        0x36314648u     /* "HF16" */
#define SOURCE_HEADER       16u
#define SOURCE_MAX          (FLASH_SECTOR_SIZE - SOURCE_HEADER)

#define STACK_MAX           20              /* ssnap in kernel.fs */

static u8 in_ring[IN_SIZE], out_ring[OUT_SIZE];
static u32 in_head, in_tail, out_head, out_tail;    /* free-running indices */
static u16 starts;
static int running;
static u16 image_checksum;

/* the saved source: header fields as stored, and the image of the whole sector */
static struct {
    u32 magic, length, crc, autostart;
} stored;
static int stored_state;                            /* 0 none, 1 stored, 2 damaged */
static u8 sector[FLASH_SECTOR_SIZE], staging[SOURCE_MAX];
static u32 feed_pos, feed_len;                      /* autostart feed out of sector[] */
static const char *last_start = "none";             /* "fed" or "skipped" */

/* the request being received: its line, then the payload of a store */
static enum { TERMINAL, LINE, PAYLOAD } rx_mode;
static char line[LINE_MAX + 1];
static u32 line_len, payload_len, payload_pos, payload_crc;
static int payload_auto;
static XTime rx_last;

/* ------------------------------------------------------------------------------------ */
/* UART and Forth's FIFOs                                                               */
/* ------------------------------------------------------------------------------------ */

static void out_put(u8 c)
{
    out_ring[out_head++ % OUT_SIZE] = c;
}

static void out_put_str(const char *s)
{
    while (*s && out_head - out_tail < OUT_SIZE)
        out_put((u8)*s++);
}

/* Forth's output into the output ring (only while it has room for a doubled SOH), the
   ring into the UART; the autostart feed and the input ring into the RX FIFO */
static void pump(void)
{
    for (int i = 0; i < 64 && OUT_SIZE - (out_head - out_tail) >= 2; i++) {
        u32 t = Xil_In32(FORTH_BASE + REG_TX_POP);
        if (!(t & TX_VALID))
            break;
        if ((u8)t == SOH)
            out_put(SOH);
        out_put((u8)t);
    }
    while (out_tail != out_head && !XUartPs_IsTransmitFull(UART))
        XUartPs_WriteReg(UART, XUARTPS_FIFO_OFFSET, out_ring[out_tail++ % OUT_SIZE]);
    while (feed_pos < feed_len && in_head - in_tail < IN_SIZE)
        in_ring[in_head++ % IN_SIZE] = sector[SOURCE_HEADER + feed_pos++];
    while (in_tail != in_head && !(Xil_In32(FORTH_BASE + REG_STATUS) & STATUS_RX_FULL))
        Xil_Out32(FORTH_BASE + REG_RX_PUSH, in_ring[in_tail++ % IN_SIZE]);
}

/* sends what Forth printed so far, then blocks on the UART */
static void send_flush(void)
{
    while (out_tail != out_head)
        XUartPs_SendByte(UART, out_ring[out_tail++ % OUT_SIZE]);
}

static void send_bytes(const void *data, u32 length)
{
    const u8 *b = data;

    while (length--)
        XUartPs_SendByte(UART, *b++);
}

/* ------------------------------------------------------------------------------------ */
/* Saved source                                                                         */
/* ------------------------------------------------------------------------------------ */

static u32 crc32(const u8 *data, u32 length)
{
    u32 crc = 0xFFFFFFFFu;

    while (length--) {
        crc ^= *data++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

/* reads the sector and checks the header and CRC */
static void source_load(void)
{
    stored_state = 0;
    if (!flash_read(FLASH_SOURCE_BASE, sector, sizeof sector))
        return;
    memcpy(&stored, sector, sizeof stored);
    if (stored.magic != SOURCE_MAGIC)
        return;
    stored_state = stored.length <= SOURCE_MAX &&
                   crc32(sector + SOURCE_HEADER, stored.length) == stored.crc ? 1 : 2;
}

/* writes header + source from sector[] and checks the flash against it */
static int source_write(u32 length)
{
    static u8 check[FLASH_SECTOR_SIZE];

    if (!flash_erase_sector(FLASH_SOURCE_BASE) ||
        !flash_program(FLASH_SOURCE_BASE, sector, SOURCE_HEADER + length) ||
        !flash_read(FLASH_SOURCE_BASE, check, SOURCE_HEADER + length) ||
        memcmp(check, sector, SOURCE_HEADER + length)) {
        source_load();
        return 0;
    }
    source_load();
    return stored_state == 1;
}

/* ------------------------------------------------------------------------------------ */
/* Forth                                                                                */
/* ------------------------------------------------------------------------------------ */

static int load_and_start(int skip_autostart)
{
    char text[96];
    int i, errors = 0;

    Xil_Out32(FORTH_BASE + REG_CTRL, CTRL_HOLD);    /* hold HF16 in reset, FIFOs cleared */
    running = 0;
    in_tail = in_head;
    feed_pos = feed_len = 0;
    last_start = "none";
    Xil_Out32(FORTH_BASE + REG_MEM_ADDR, 0);
    for (i = 0; i < FORTH_IMAGE_WORDS; i++)
        Xil_Out32(FORTH_BASE + REG_MEM_DATA, forth_image[i]);

    for (i = 0; i < FORTH_IMAGE_WORDS; i++) {
        Xil_Out32(FORTH_BASE + REG_MEM_ADDR, i);
        if ((Xil_In32(FORTH_BASE + REG_MEM_DATA) & 0xFFFF) != forth_image[i])
            errors++;
    }

    starts++;
    if (errors) {
        snprintf(text, sizeof text, "\r\n[zynq-forth] kernel image VERIFY FAILED (%d words), "
                 "CPU stays in reset\r\n", errors);
        out_put_str(text);
        return 0;
    }
    snprintf(text, sizeof text, "\r\n[zynq-forth] %s, kernel %d words loaded and verified\r\n",
             FIRMWARE_VERSION, FORTH_IMAGE_WORDS);
    out_put_str(text);

    if (stored_state == 1 && stored.autostart) {
        if (skip_autostart || (Xil_In32(FORTH_BASE + REG_STATUS) & STATUS_BTN3)) {
            last_start = "skipped";
            out_put_str("[zynq-forth] autostart skipped\r\n");
        } else {
            last_start = "fed";
            feed_len = stored.length;
            snprintf(text, sizeof text, "[zynq-forth] autostart: %lu bytes of saved source\r\n",
                     (unsigned long)stored.length);
            out_put_str(text);
        }
    }
    Xil_Out32(FORTH_BASE + REG_CTRL, 0);            /* run */
    running = 1;
    return 1;
}

static u16 ram_read(u16 byte_address)
{
    Xil_Out32(FORTH_BASE + REG_MEM_ADDR, (byte_address >> 1) & 0x1FFF);
    return Xil_In32(FORTH_BASE + REG_MEM_DATA) & 0xFFFF;
}

static u8 ram_byte(u16 byte_address)
{
    u16 w = ram_read(byte_address);
    return byte_address & 1 ? w >> 8 : w & 0xFF;
}

static void cpu_pause(void)
{
    Xil_Out32(FORTH_BASE + REG_CTRL, running ? CTRL_PAUSE : CTRL_HOLD);
}

static void cpu_resume(void)
{
    Xil_Out32(FORTH_BASE + REG_CTRL, running ? 0 : CTRL_HOLD);
}

/* reads the stack QUIT copied at the last prompt; SDEPTH is -1 while the copy is made,
   so a pause in the middle lets the CPU run 10 us and reads again */
static int stack_snapshot(u16 *items)
{
    XTime start, now;

    for (int attempt = 0; attempt < 100; attempt++) {
        u16 depth;

        cpu_pause();
        depth = ram_read(FORTH_SDEPTH_ADDR);
        if (depth <= STACK_MAX)
            for (int i = 0; i < depth; i++)
                items[i] = ram_read(FORTH_SSNAP_ADDR + 2 * i);
        cpu_resume();
        if (depth <= STACK_MAX)
            return depth;
        XTime_GetTime(&start);
        do
            XTime_GetTime(&now);
        while (now - start < COUNTS_PER_SECOND / 100000);
    }
    return -1;
}

/* ------------------------------------------------------------------------------------ */
/* Service requests                                                                     */
/* ------------------------------------------------------------------------------------ */

static u8 reply[FLASH_SECTOR_SIZE + 4096];         /* largest: the saved source */

static void answer(const char *name, const void *payload, u32 length)
{
    char head[64];

    send_flush();
    snprintf(head, sizeof head, "%cok %s %lu\n", SOH, name, (unsigned long)length);
    send_bytes(head, strlen(head));
    send_bytes(payload, length);
}

static void refuse(const char *name, const char *reason)
{
    char head[64];

    send_flush();
    snprintf(head, sizeof head, "%cerr %s %s\n", SOH, name, reason);
    send_bytes(head, strlen(head));
}

static void request_version(void)
{
    int n = snprintf((char *)reply, sizeof reply,
                     "firmware zynq-forth %s\nprotocol %d\nkernel %d %04X\n",
                     FIRMWARE_VERSION, PROTOCOL_VERSION, FORTH_IMAGE_WORDS, image_checksum);
    answer("version", reply, n);
}

static void request_status(void)
{
    u32 pending = (in_head - in_tail) + (feed_len - feed_pos);
    int n = snprintf((char *)reply, sizeof reply,
                     "state %s\npc %04lX\ninput %lu\nstarts %u\nsource %s\nlength %lu\n"
                     "crc %08lX\nautostart %s\nlast %s\n",
                     running ? "running" : "held",
                     (unsigned long)((Xil_In32(FORTH_BASE + REG_STATUS) >> 16) & 0x1FFF),
                     (unsigned long)pending, starts,
                     stored_state == 1 ? "stored" : stored_state == 2 ? "damaged" : "none",
                     (unsigned long)(stored_state == 1 ? stored.length : 0),
                     (unsigned long)(stored_state == 1 ? stored.crc : 0),
                     stored_state == 1 && stored.autostart ? "on" : "off", last_start);
    answer("status", reply, n);
}

/* walks the header list while the CPU is paused; links must point downwards */
static void request_words(void)
{
    u32 n;
    u16 h;

    cpu_pause();
    n = snprintf((char *)reply, sizeof reply, "here %04X\n", ram_read(FORTH_DP_ADDR));
    for (h = ram_read(FORTH_LATEST_ADDR); h != 0 && n < sizeof reply - 64; ) {
        u8 count = ram_byte(h + 2);
        u8 length = count & 0x1F;
        u16 link = ram_read(h);
        char flags[4];
        int f = 0;

        if (count & 0x80) flags[f++] = 'i';         /* immediate */
        if (count & 0x40) flags[f++] = 'h';         /* hidden */
        if (count & 0x20) flags[f++] = 'n';         /* inline */
        if (!f) flags[f++] = '-';
        flags[f] = 0;
        n += snprintf((char *)reply + n, sizeof reply - n, "%04X %s ",
                      (h + 3 + length + 1) & ~1u, flags);
        for (int k = 0; k < length; k++)
            reply[n++] = ram_byte(h + 3 + k);
        reply[n++] = '\n';
        if (link >= h)
            break;
        h = link;
    }
    cpu_resume();
    answer("words", reply, n);
}

static void request_stack(void)
{
    u16 items[STACK_MAX];
    int depth = stack_snapshot(items);
    u32 n;

    if (depth < 0) {
        refuse("stack", "busy");
        return;
    }
    n = snprintf((char *)reply, sizeof reply, "%d", depth);
    for (int i = 0; i < depth; i++)
        n += snprintf((char *)reply + n, sizeof reply - n, " %04X", items[i]);
    reply[n++] = '\n';
    answer("stack", reply, n);
}

static void request_board(void)
{
    u32 n = snprintf((char *)reply, sizeof reply, "board %08lX\nports",
                     (unsigned long)Xil_In32(FORTH_BASE + REG_BOARD));

    for (int p = 0; p < 3; p++)
        n += snprintf((char *)reply + n, sizeof reply - n, " %08lX",
                      (unsigned long)Xil_In32(FORTH_BASE + REG_PORT0 + 4 * p));
    n += snprintf((char *)reply + n, sizeof reply - n, "\npwm %08lX\nxadc",
                  (unsigned long)Xil_In32(FORTH_BASE + REG_PWM));
    for (int r = 0; r < 32; r++) {
        Xil_Out32(FORTH_BASE + REG_XADC, r);
        n += snprintf((char *)reply + n, sizeof reply - n, " %04lX",
                      (unsigned long)(Xil_In32(FORTH_BASE + REG_XADC) & 0xFFFF));
    }
    reply[n++] = '\n';
    answer("board", reply, n);
}

static void request_source(void)
{
    if (stored_state != 1)
        refuse("source", stored_state == 2 ? "damaged" : "none");
    else
        answer("source", sector + SOURCE_HEADER, stored.length);
}

static void finish_store(void)
{
    if (crc32(staging, payload_len) != payload_crc) {
        refuse("store", "crc");
        return;
    }
    send_flush();                                   /* the flash takes up to ~3 s */
    stored.magic = SOURCE_MAGIC;
    stored.length = payload_len;
    stored.crc = payload_crc;
    stored.autostart = payload_auto;
    memcpy(sector, &stored, sizeof stored);
    memcpy(sector + SOURCE_HEADER, staging, payload_len);
    if (source_write(payload_len))
        answer("store", 0, 0);
    else
        refuse("store", "flash");
}

/* splits the line into at most four words */
static int split(char *s, char *argv[4])
{
    int argc = 0;

    for (char *t = strtok(s, " \t"); t && argc < 4; t = strtok(0, " \t"))
        argv[argc++] = t;
    return argc;
}

static void request(char *text)
{
    char *argv[4];
    int argc = split(text, argv);
    char *end;

    if (argc == 0)
        return;
    if (!strcmp(argv[0], "echo")) {
        answer("echo", argc > 1 ? argv[1] : "", argc > 1 ? strlen(argv[1]) : 0);
    } else if (!strcmp(argv[0], "version")) {
        request_version();
    } else if (!strcmp(argv[0], "status")) {
        request_status();
    } else if (!strcmp(argv[0], "restart")) {
        int skip = argc > 1 && !strcmp(argv[1], "skip");
        if (load_and_start(skip))
            answer("restart", 0, 0);
        else
            refuse("restart", "verify");
    } else if (!strcmp(argv[0], "words")) {
        request_words();
    } else if (!strcmp(argv[0], "stack")) {
        request_stack();
    } else if (!strcmp(argv[0], "board")) {
        request_board();
    } else if (!strcmp(argv[0], "source")) {
        request_source();
    } else if (!strcmp(argv[0], "store")) {
        if (argc != 4) {
            refuse("store", "args");
            return;
        }
        payload_len = strtoul(argv[1], &end, 10);
        payload_crc = strtoul(argv[2], 0, 16);
        payload_auto = !strcmp(argv[3], "on");
        if (*end || payload_len > SOURCE_MAX) {
            refuse("store", "length");
            return;
        }
        payload_pos = 0;
        if (payload_len)
            rx_mode = PAYLOAD;
        else
            finish_store();
    } else if (!strcmp(argv[0], "erase")) {
        send_flush();
        if (flash_erase_sector(FLASH_SOURCE_BASE) && (source_load(), stored_state == 0))
            answer("erase", 0, 0);
        else
            refuse("erase", "flash");
    } else if (!strcmp(argv[0], "autostart")) {
        if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
            refuse("autostart", "args");
        } else if (stored_state != 1) {
            refuse("autostart", "none");
        } else {
            send_flush();
            stored.autostart = !strcmp(argv[1], "on");
            memcpy(sector, &stored, sizeof stored);
            if (source_write(stored.length))
                answer("autostart", 0, 0);
            else
                refuse("autostart", "flash");
        }
    } else {
        refuse(argv[0], "unknown");
    }
}

/* a request that stops arriving for 2 s is dropped */
static void receive_timeout(void)
{
    XTime now;

    XTime_GetTime(&now);
    if (rx_mode != TERMINAL && now - rx_last > REQUEST_TIMEOUT) {
        if (rx_mode == PAYLOAD)
            refuse("store", "timeout");
        rx_mode = TERMINAL;
    }
}

/* one byte from the UART: terminal input, or part of a service request */
static void receive(u8 c)
{
    receive_timeout();
    XTime_GetTime(&rx_last);

    switch (rx_mode) {
    case TERMINAL:
        if (c == SOH) {
            rx_mode = LINE;
            line_len = 0;
        } else if (c == CTRL_R) {
            load_and_start(0);
        } else if (in_head - in_tail < IN_SIZE) {
            in_ring[in_head++ % IN_SIZE] = c;
        }
        break;
    case LINE:
        if (c == '\n') {
            line[line_len] = 0;
            rx_mode = TERMINAL;
            request(line);
        } else if (c != '\r' && line_len < LINE_MAX) {
            line[line_len++] = c;
        }
        break;
    case PAYLOAD:
        staging[payload_pos++] = c;
        if (payload_pos == payload_len) {
            rx_mode = TERMINAL;
            finish_store();
        }
        break;
    }
}

int main(void)
{
    XTime start, now;

    for (int i = 0; i < FORTH_IMAGE_WORDS; i++)
        image_checksum += forth_image[i];

    /* the buttons are sampled every 10 ms; let BTN3 settle before deciding the autostart */
    XTime_GetTime(&start);
    do
        XTime_GetTime(&now);
    while (now - start < COUNTS_PER_SECOND / 20);

    if (flash_init())
        source_load();
    load_and_start(0);

    for (;;) {
        while (XUartPs_IsReceiveData(UART))
            receive(XUartPs_ReadReg(UART, XUARTPS_FIFO_OFFSET));
        receive_timeout();
        pump();
    }
}
