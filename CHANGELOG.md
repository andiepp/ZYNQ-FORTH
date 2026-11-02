# Changelog

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
