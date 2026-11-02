# HF16 — a 16-bit Forth CPU for the Arty Z7-20

HF16 is a small dual-stack machine in the PL whose instructions are Forth primitives.
A Forth kernel, cross-compiled from `forth/kernel.fs` by `tools/hf16.py`, gives an
interactive Forth over the USB UART. The Cortex-A9 (`sw/src`) loads the kernel image and
moves characters between the PS UART and FIFOs in the PL; it also answers the app's service
requests ([Protocol.md](Protocol.md)) itself.

```
 ZYNQ-FORTH app ── USB-UART ── PS UART0 ── ARM program ──── AXI-Lite ── forth_system (PL)
 (or PuTTY)
                                                             ├─ HF16 CPU
                                                             ├─ 16 KB RAM (BRAM)
                                                             ├─ RX/TX FIFOs
                                                             ├─ LEDs, RGB, buttons, switches, ms timer
                                                             ├─ shield pins: 36 GPIO (rtl/forth_system.v)
                                                             ├─ PWM: 4 channels on any shield pin (rtl/pwm.v)
                                                             └─ XADC scanner: A0..A11, temperature, supplies
                                                                (rtl/xadc_scan.v)
```

## Machine model

- 16-bit cells. Byte addresses; cells are little-endian and must be even-aligned.
- 16 KB RAM at 0x0000–0x3FFF holds code and data (code can be compiled at run time).
- I/O at 0x8000 and up (address bit 15 set).
- Data stack: T register plus 32 entries (N is the top entry). Return stack: 32 entries.
- The return stack holds **byte** addresses; the PC is a word address (`pc = R >> 1`).
- One instruction per clock.

## Instruction encoding

| Bits 15..13 | Meaning |
|---|---|
| `1xx` | literal: push `insn[14:0]` (0..32767) |
| `000` | jump to word address `insn[12:0]` |
| `001` | 0branch: pop T, jump to `insn[12:0]` if it was zero |
| `010` | call `insn[12:0]`, pushing the byte address of the next instruction on R |
| `011` | ALU instruction, fields below |

ALU instruction fields:

| Bits | Field | Effect |
|---|---|---|
| 12 | R2P | return: `pc = R >> 1` (combine with rd = -1) |
| 11..7 | op | new T, table below |
| 6 | T2N | write old T into N (used by SWAP; implied on a data push) |
| 5 | N2M | store N at address T (RAM or I/O) |
| 4 | BYTE | the store writes one byte instead of a cell |
| 3..2 | rd | return stack delta: 00 = 0, 01 = +1 (pushes T), 11 = -1, 10 = -2 |
| 1..0 | dd | data stack delta: 00 = 0, 01 = +1 (pushes T into N), 11 = -1, 10 = -2 |

| op | new T | op | new T |
|---|---|---|---|
| 0 | T | 12 | R |
| 1 | N | 13 | cell at [T] |
| 2 | N + T | 14 | byte at [T] |
| 3 | N and T | 15 | data stack depth |
| 4 | N or T | 16 | N − T |
| 5 | N xor T | 17 | T + 1 |
| 6 | not T | 18 | T − 1 |
| 7 | N = T (−1/0) | 19 | T = 0 (−1/0) |
| 8 | N < T signed | 20 | T < 0 (−1/0) |
| 9 | N < T unsigned | 21 | N × T (low 16 bits) |
| 10 | N >> T logical | 22 | T >> 1 arithmetic |
| 11 | N << T | 23 | −T |
| | | 24 | N × T unsigned, high 16 bits (for `um*`) |

**Store hazard:** a store cycle uses the RAM data port for the write, so the instruction
directly after a store must not read memory (ops 13/14). The compilers always follow a
store with a plain DROP, which satisfies this.

## I/O map

| Address | Read | Write |
|---|---|---|
| 0x8000 | next received byte (pops the RX FIFO) | send byte (pushes the TX FIFO) |
| 0x8002 | bit 0 = RX byte available, bit 1 = TX FIFO has space | – |
| 0x8004 | LD0..LD3 | LD0..LD3 |
| 0x8006 | bits 3..0 = BTN3..BTN0 (debounced), bits 5..4 = SW1..SW0 | – |
| 0x8008 | RGB LEDs: bits 2..0 = LD4 B,G,R; bits 5..3 = LD5 B,G,R | same |
| 0x800A | millisecond counter (free running, wraps) | – |
| 0x8010 + 8p | port p (0..2) pin levels, synchronized; pin n = bit n%16 of port n/16 | – |
| 0x8012 + 8p | port p output latch | same |
| 0x8014 + 8p | port p direction, 1 = output (all 0 after reset) | same |
| 0x8040 + 2r | XADC status register r (0..31), result in bits 15..4 | – |
| 0x8080 + 8c | PWM channel c (0..3): period, low 16 bits | same (held until the high half is written) |
| 0x8082 + 8c | PWM channel c: period, high 16 bits (clocks per cycle, at least 2) | same (takes both halves) |
| 0x8084 + 8c | PWM channel c: duty in 1/65536 of the period, 0xFFFF = always high | same |
| 0x8086 + 8c | PWM channel c: bit 15 on, bits 5..0 pin number | same |

Only address bits 7..1 are decoded, so the map repeats every 256 bytes above 0x8000.

### Shield pins

| Pin numbers | Port bits | Shield | FPGA port | Pull-up |
|---|---|---|---|---|
| 0..13 | port 0 bits 0..13 | IO0..IO13 | `ck_io[13:0]` | yes |
| 14..19 | port 0 bits 14..15, port 1 bits 0..3 | A0..A5 as digital pins | `ck_a[5:0]` | no (shares the analog divider node) |
| 26..41 | port 1 bits 10..15, port 2 bits 0..9 | IO26..IO41 | `ck_ioh[15:0]` | yes |

Pins 20..25 and 42..47 do not exist: they read 0 and ignore writes. The Forth word `pin`
rejects them.

### PWM

`rtl/pwm.v` has four channels. Each counts 50 MHz clocks from 0 to period − 1 and drives
its output high for the first (period × duty) / 65536 clocks; the 32 × 16 product is
pipelined in two stages (a DSP48 per channel). New period and duty values are taken at the
end of the running cycle, so no pulse is cut short; enabling starts a fresh cycle. An
enabled channel owns its pin: `forth_system` drives the pin as an output with the channel's
level (channels on the same pin are ORed); disabled, the pin returns to its latch and
direction. Reset (also a CPU restart) turns every channel off. The ISS keeps the registers
but has no time base: a PWM pin reads high there only at full duty.

### XADC

`xadc_scan` instantiates the XADC primitive in continuous sequence mode (ADCCLK = 12.5 MHz,
16-sample averaging, unipolar, extended settling on the auxiliary inputs). A scanner
reads status registers 0x00..0x1F over the DRP one after another into a 32 × 16
register file, which the CPU reads without waiting. The first DRP read is held back 4096
clocks after configuration; a request during the XADC's own reset is never answered.

| Forth channel | Shield | XADC input | Status register | Volts at the XADC |
|---|---|---|---|---|
| 0..5 | A0..A5 | VAUX 1, 9, 6, 15, 5, 13 | 0x11, 0x19, 0x16, 0x1F, 0x15, 0x1D | shield voltage / 3.32 (2.32k/1k divider) |
| 6 | A6 (+), A7 (−) | VAUX 12 | 0x1C | A6 − A7, 0..1 V |
| 7 | A8 (+), A9 (−) | VAUX 0 | 0x10 | A8 − A9, 0..1 V |
| 8 | A10 (+), A11 (−) | VAUX 8 | 0x18 | A10 − A11, 0..1 V |

On-chip sensors in the sequence: 0x00 temperature (°C = code × 503.975 / 4096 − 273.15),
0x01 VCCINT, 0x02 VCCAUX, 0x06 VCCBRAM, 0x0D VCCPINT, 0x0E VCCPAUX, 0x0F VCCO_DDR
(volts = code × 3 / 4096).

In simulation the Xilinx XADC model reads its inputs from `sim/xadc_stim.txt`;
`tools/hf16.py` uses the same values (table `XADC_SIM`).

## AXI-Lite registers (ARM side)

| Offset | Register |
|---|---|
| 0x00 | CTRL: bit 0 = hold CPU in reset (1 after PL reset), bit 1 = pause the CPU |
| 0x04 | MEM_ADDR: RAM word address for the loader |
| 0x08 | MEM_DATA: write stores at MEM_ADDR and increments it (only while in reset); read returns the word at MEM_ADDR (while in reset or paused) |
| 0x0C | TX_POP: bit 8 = valid, bits 7..0 = byte from the CPU; reading pops it |
| 0x10 | RX_PUSH: write a byte for the CPU |
| 0x14 | STATUS: bit 0 = RX FIFO full, bit 1 = TX FIFO not empty, bits 7..4 = BTN3..BTN0 and bits 9..8 = SW1..SW0 (debounced, as at 0x8006), bits 28..16 = CPU PC |
| 0x18 | BOARD: bits 3..0 = LD3..LD0, bits 9..4 = RGB (as at 0x8008), bits 19..16 = BTN3..BTN0, bits 21..20 = SW1..SW0 |
| 0x1C, 0x20, 0x24 | PORT0..PORT2: bits 15..0 = pin levels (as at 0x8010 + 8p), bits 31..16 = pins driven (direction or PWM) |
| 0x28 | PWM: bits 3..0 = channel enables, then the pin of channel c in bits 9+6c..4+6c |
| 0x2C | XADC: write selects a status register 0..31; read returns bits 20..16 = the selection, bits 15..0 = its value (the scanner's second read port) |

Registers 0x18 to 0x2C let the ARM show the board's state in the app while Forth runs; they
only read, so they never disturb the CPU.

**Pause.** CTRL bit 1 freezes the CPU between two instructions: no register changes, no
store, no I/O side effects, and RAM port B goes to MEM_ADDR/MEM_DATA, so the ARM can read
RAM while a program runs. When the bit is cleared the CPU stays stalled one more cycle,
in which port B re-reads the CPU's [T] and the code port the current instruction; then it
continues as if nothing had happened (except that the ms timer kept running). The RTL
testbench pauses at random moments throughout its runs and checks that the output does
not change.

## The ARM program (`sw/src`)

Bare metal on Cortex-A9 core 0, polling. At start it waits 50 ms (so the debounced BTN3 is
valid), reads the saved source from flash, loads and verifies the kernel image and releases
the CPU. Then it loops:

- UART bytes go to the input ring (64 KB) unless they belong to a service request (SOH ...
  LF, see [Protocol.md](Protocol.md)); Ctrl-R restarts Forth.
- The input ring and the autostart feed go into the RX FIFO whenever it has room.
- Forth's TX FIFO goes into a 4 KB output ring, which drains into the UART as fast as it
  sends. When the ring is full the TX FIFO stays full and Forth waits in `emit`, so no output
  is lost. A 0x01 from Forth is doubled on the way out.
- An answer first flushes the output ring, then goes out whole, so it never splits Forth's
  output.

**Saved source.** One source, at most 65,520 bytes, lives in the 64 KB flash sector at
0xFF0000, far above `BOOT.bin`, behind a 16-byte header (magic `HF16`, length, CRC-32 of
the text, autostart flag; little-endian). Lines end in CR, as typed at the terminal. At
power-up and at every restart, if autostart is on, the ARM feeds the source into Forth's
input right after the kernel, as if it were typed; holding **BTN3** during the start or
`restart skip` skips it. While the flash is erased or programmed (up to ~3 s) nothing moves,
so Forth may wait on a full TX FIFO for that time.

**Dictionary.** The ARM pauses the CPU (CTRL bit 1), reads `latest` and `dp` (their
addresses are in `build/forth_image.h`, written by `tools/hf16.py`), follows the header
links downwards and formats the answer, then resumes the CPU.

**Data stack.** `quit` copies the stack into `ssnap` before each line, with `sdepth` = -1
while it copies. The ARM pauses the CPU and reads `sdepth` and the items; if it caught the
copy half-done, it lets the CPU run 10 µs and tries again (up to 100 times).

## Dictionary layout

```
header:  link (cell, address of previous header, 0 ends the list)
         count byte: bits 4..0 = name length, bit 7 = IMMEDIATE, bit 6 = HIDDEN
         name bytes, padded to an even address
xt:      code
```
