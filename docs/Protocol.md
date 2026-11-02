# ZYNQ-FORTH serial protocol

Protocol version 1, firmware 1.0.

The board's USB serial port (the Arty Z7's FTDI channel B, 115200 baud, 8N1, no handshake)
carries two things at once:

- **Forth's terminal**, as plain bytes in both directions. A terminal program such as PuTTY
  or Tera Term talks to Forth directly: type a line, press Enter (CR), and Forth answers.
- **Service requests** from the ZYNQ-FORTH app (or your own program): version, status, the
  dictionary, the data stack, the board's I/O state and the source saved in flash. They are
  set apart by the byte SOH (0x01), which Forth never needs.

The ARM core answers service requests itself, while Forth keeps running: a request never
waits for Forth, and it works even while a Forth word loops for ever.

## Terminal

| Direction | Bytes |
|---|---|
| PC → board | Every byte except SOH (0x01) and Ctrl-R (0x12) goes to Forth's input. Lines end with CR. Up to 64 KB wait in the ARM's input ring while Forth is busy; more is lost. |
| PC → board | **Ctrl-R** (0x12) reloads the kernel and restarts Forth (with the saved source, if autostart is on): the way out of an endless loop from a terminal program. |
| board → PC | Forth's output. Forth echoes what it reads and ends lines with CR LF. A 0x01 that Forth prints itself (`1 emit`) is sent doubled, 0x01 0x01. |

At every start the ARM prints a line before Forth's banner:

```
[zynq-forth] 1.0, kernel 2398 words loaded and verified
[zynq-forth] autostart: 195 bytes of saved source
```

## Requests and answers

A request is SOH, one line of ASCII text, LF:

```
01 "status" 0A
```

Words are separated by single spaces. `store` is followed by a payload of the stated length.
Send the next request only after the answer to the previous one; terminal bytes may be sent
at any time between requests.

An answer is SOH, a header line ending in LF and, for `ok`, a payload:

```
01 "ok <request> <length>" 0A <length bytes>
01 "err <request> <reason>" 0A
```

The answer is never mixed into Forth's output: it comes between two bytes of it. A reader
splits the stream like this:

1. A byte other than SOH is terminal output.
2. SOH SOH is the terminal byte 0x01.
3. SOH followed by anything else starts an answer: read the header line up to LF; if it
   starts with `ok`, read the payload of the given length.

A request that is not completed within 2 seconds (a missing LF, a short payload) is
dropped; an unfinished `store` answers `err store timeout`.

## Requests

Text payloads are lines ending in LF. Numbers are decimal unless they are shown as hex.

| Request | Payload of the `ok` answer | Refusals |
|---|---|---|
| `echo <word>` | the word | – |
| `version` | `firmware zynq-forth 1.0`, `protocol 1`, `kernel <cells> <checksum hex>` (the kernel image's size in 16-bit cells and the sum of its cells) | – |
| `status` | see below | – |
| `restart` | empty; reloads and verifies the kernel and starts Forth, which then prints its banner and runs the saved source if autostart is on | `verify`: the image read back wrong, the CPU stays in reset |
| `restart skip` | the same without the saved source | `verify` |
| `words` | the dictionary, see below | – |
| `stack` | the data stack as it was at the last prompt, see below | `busy`: Forth was copying it; ask again |
| `board` | the board's I/O state, see below | – |
| `source` | the saved source, exactly as stored (lines end in CR) | `none`, `damaged` |
| `store <length> <crc32 hex> on\|off` + payload | empty; saves the payload in flash with the autostart flag. The CRC is CRC-32 as zlib computes it (`CBF43926` for `123456789`). Takes up to about 3 s. | `args`, `length` (more than 65,520 bytes), `crc`, `flash`, `timeout` |
| `erase` | empty; erases the saved source | `flash` |
| `autostart on\|off` | empty; changes the flag of the saved source | `args`, `none`, `flash` |

Any other request is answered `err <request> unknown`.

### status

One `key value` per line:

```
state running        running, or held (the kernel failed its check)
pc 02BC              the HF16 program counter (hex, word address)
input 0              bytes waiting for Forth: input ring plus the rest of the saved source
starts 1             starts since power-up (power-up, restarts, Ctrl-R)
source stored        none, stored or damaged (its CRC does not match)
length 195           the saved source's length in bytes (0 unless stored)
crc AA4BEAA3         its CRC-32 (hex)
autostart on         on or off
last fed             the last start: fed (saved source ran), skipped (BTN3 or restart skip), none
```

### words

The first line is `here XXXX`, the next free byte of the dictionary in hex. Then one line
per word, newest first: the code address in hex, the flags and the name.

```
here 1324
1310 - hello
0F82 i if
0040 n dup
```

Flags are `-` (none) or letters: `i` immediate, `h` hidden (being defined), `n` inline. The
ARM pauses the HF16 for the few microseconds the walk takes.

### stack

One line: the depth, then each item as four hex digits, bottom first.

```
3 0001 FFFE 012C          1 -2 300, with 300 on top
```

Forth's `quit` copies the stack before it reads each line, so this is the stack at the
last ` ok`, the same `.s` would have shown there. At most 20 items.

### board

Raw register values in hex; the layout is in [design.md](design.md#axi-lite-registers-arm-side).

```
board 00120115                LEDs, RGB LEDs, buttons, switches
ports 2000FFFB 08000000 00000000
pwm 00002C02
xadc A11D 56ED 998F 0000 …    32 XADC status registers
```

- `board`: bits 3..0 = LD3..LD0, bits 9..4 = RGB (bits 4/5/6 = LD4 red/green/blue,
  7/8/9 = LD5 red/green/blue), bits 19..16 = BTN3..BTN0, bits 21..20 = SW1..SW0.
- `ports`: three words for pins 0–15, 16–31 and 32–47. Bits 15..0 are the pin levels as read
  at the pins, bits 31..16 say which pins are driven (an output, or a PWM channel's pin).
  Pin numbers as in Forth: 0–13 = IO0–IO13, 14–19 = A0–A5, 26–41 = IO26–IO41.
- `pwm`: bits 3..0 = channels 3..0 enabled, then six bits per channel with its pin number
  (channel c at bits 4+6c..9+6c).
- `xadc`: the XADC status registers 0x00–0x1F; the result is in bits 15..4. Temperature
  (register 0) in °C = code × 503.975 / 4096 − 273.15; supplies (1, 2, 6, 0x0D–0x0F) in volts
  = code × 3 / 4096; A0–A5 = registers 0x11, 0x19, 0x16, 0x1F, 0x15, 0x1D with 3.32 V full
  scale; A6/A7, A8/A9, A10/A11 = 0x1C, 0x10, 0x18 with 1 V full scale.

## Example in Python

`tools/zforth.py` is a complete client (it needs pyserial):

```
python tools/zforth.py -p COM17 version status "send:2 3 + ." stack words
python tools/zforth.py -p COM17 store:lights.fs:on restart wait:2
```

The reading side in a few lines:

```python
import serial
port = serial.Serial('COM17', 115200, timeout=2)
port.write(b'\x01status\n')
while port.read(1) != b'\x01':          # skip Forth's output
    pass
head = port.readline().decode().split()  # ['ok', 'status', '99']
print(port.read(int(head[2])).decode())
```
