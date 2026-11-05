# Changelog

## 2.4.2 — 2026-11-05

- App: the **Manual** button (F1) opens the MAX1000-FORTH manual when the board of the
  current or last connection is a MAX1000; the app carries a copy of it
  (`docs/max1000/MAX1000-FORTH-Manual.pdf`, from MAX1000-FORTH 1.0).
- Firmware, bitstream and kernel are unchanged (2.3).

## 2.4.1 — 2026-11-05

- App: the MAX1000 board view shows the chip temperature and the accelerometer's tilt (the
  `temp` and `accel` lines of MAX1000-FORTH 1.0) and no longer a fixed clock/RAM label.
- Docs: README and manual section 2.11 link the now public
  [MAX1000-FORTH](https://github.com/andiepp/MAX1000-FORTH) project; section 5.8's hex
  example corrected (`255 hex .` prints `FF`).
- Firmware, bitstream and kernel are unchanged (2.3).

## 2.4 — 2026-11-05

- App: also talks to **MAX1000-FORTH**, the HF16 Forth on the Trenz MAX1000 (a separate
  project), which answers protocol 1 from its Forth kernel. The `version` answer's firmware
  name selects the board: a MAX1000 board view (8 LEDs, user button, D0–D14, PIO1–PIO8,
  AIN0–AIN7 and AIN), no Pattern tab, restart by Ctrl-R (plus ESC to skip the saved source),
  and text sent in 4 KB pieces. The app remembers the board type of the last connection.
  ZYNQ-FORTH boards behave as before.
- Manual: section 2.11, the app with a MAX1000. Protocol.md: section MAX1000-FORTH.
- Firmware, bitstream and kernel are unchanged (2.3); the firmware zip carries the 2.3
  `BOOT.bin`.

## 2.3 — 2026-11-03

- Firmware 2.3: HASE mode gains device function 0x44, the pattern generator's registers
  (the `board` answer's `pg` line, 8 bytes), for the HASE Forth-Lab panel, which now has
  the app's Pattern tab and Pmod view (HASE ADR-0090). Bitstream, kernel and the app's
  protocol unchanged; the app carries the version number 2.3 with the release.

## 2.2.2 — 2026-11-03

- App: the timing diagram starts with an **idle** column: the idle levels, switched by a
  click, and the edge from them into step 0; it replaces the idle check boxes. The channel
  list's enable boxes get an "on" header.
- Scripts: `sim/run_sim.ps1`, `sim/run_patgen.ps1`, `sw/build_boot.ps1` and
  `sw/program_flash.ps1` find Vivado / Vitis 2023.2 themselves (`XILINX_VIVADO`,
  `XILINX_VITIS`, or `\Xilinx\…\2023.2` on any drive) instead of a fixed `I:\Xilinx`.
- Manual, README, Protocol.md: the serial port written as COMx, paths relative to the
  ZYNQ-FORTH folder, the release's flash script, and when editor settings reach the board.
- Firmware, bitstream and kernel unchanged (2.2, the same BOOT.bin).

## 2.2.1 — 2026-11-03

- App 2.2.1: the board view draws the Pmod headers JA and JB as they look on the board,
  3.3 V and GND at the left, then pins 4..1 over 10..7 (2.2 had them mirrored).
- Manual, README and docs/PatternGenerator.md: the pinout diagram in the same orientation.
- Firmware, bitstream and kernel unchanged (2.2, the same BOOT.bin).

## 2.2 — 2026-11-02

- Pattern generator (docs/PatternGenerator.md): 16 channels on Pmod JA and JB, played in
  5 ns steps from its own 200 MHz clock; 4096 steps with loops nested 4 deep, jumps, waits
  for a trigger (shield pin, button or software) and an end. New bitstream (`rtl/patgen.v`,
  I/O registers 0x80C0–0x80D4, AXI registers 0x30/0x34).
- Kernel: words `ns` `us` `msec` `pg-at` `pg-step` `pg-loop` `pg-jump` `pg-wait` `pg-end`
  `pg-start` `pg-stop` `pg-trig` `pg-enable` `pg-idle` `pg-trigger` `pg-status` `pg-pc`
  `pg-out@` `pg-sync`.
- Firmware 2.2: the `board` answer gains a `pg` line (protocol 1; older apps ignore it).
- App 2.2: a **Pattern** tab beside the source editor: a timing diagram of the 16 channels
  (click or drag to set levels, select steps in the header), durations in ns/µs/ms/s,
  repeats (nested up to four deep), wait steps, trigger source and edge, what happens at the
  end, channel names, enables and idle levels; it checks the pattern against the generator's
  limits and timing rule, loads it as Forth (**Load & start**) or puts the Forth into the
  source editor. Patterns are saved as `.pattern` files (examples in `examples/patterns`).
  The board view shows Pmod JA/JB as the generator drives them and its state.
- Manual: section 2.6 (the pattern editor), 5.14 (the pattern generator, with the Pmod
  JA/JB pinout), 6.3 (examples) and troubleshooting rows; the pinout also in the README.

## 2.1 — 2026-11-02

- Firmware 2.1: HASE mode gains device function 0x43, the board registers the `board`
  request reads (LEDs, buttons, switches, shield pins, PWM, XADC), for the HASE panel's
  board view. Everything else unchanged; the app carries the version number 2.1 with the
  release.

## 2.0 — 2026-11-02

- Firmware 2.0: one firmware for both uses. Besides the terminal and the app's service
  requests (protocol 1, unchanged), the board is the HF16 Forth node of the HASE laboratory
  and switches to HASE's MCNF protocol by itself when a HASE host talks to it, and back when
  a terminal or the app does (docs/Protocol.md, "HASE mode").
- Bitstream and kernel unchanged. The app is unchanged as well and carries the version
  number 2.0 with the release; it works with firmware 1.0 and 2.0 alike.

## 1.1 — 2026-11-02

- App 1.1: a **Manual** button (and F1) opens the user manual as a PDF; the manual is built
  into the app.
- Firmware and bitstream unchanged (firmware 1.0, protocol 1): a board running 1.0 needs no
  update.

## 1.0 — 2026-11-02

First version, derived from the HF16 Forth laboratory.

- Firmware 1.0: Forth's terminal as plain bytes on the USB serial port (any terminal
  program works), service requests set apart by SOH (protocol 1: version, status, restart,
  words, stack, board, source, store, erase, autostart, echo), Ctrl-R restarts Forth.
- Bitstream: read-only board-state registers for the app (LEDs, RGB, buttons, switches,
  shield pins, PWM, a second XADC read port).
- App 1.0: editor, terminal, data stack, dictionary, saved source, live board view; skins
  IDE dark, IDE light, Green phosphor, Amber, Lab panel.
- `tools/zforth.py`, a command-line client.
