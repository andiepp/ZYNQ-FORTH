# Changelog

## Unreleased — 1.0

First version, derived from the HF16 Forth laboratory.

- Firmware 1.0: Forth's terminal as plain bytes on the USB serial port (any terminal
  program works), service requests set apart by SOH (protocol 1: version, status, restart,
  words, stack, board, source, store, erase, autostart, echo), Ctrl-R restarts Forth.
- Bitstream: read-only board-state registers for the app (LEDs, RGB, buttons, switches,
  shield pins, PWM, a second XADC read port).
- App 1.0: editor, terminal, data stack, dictionary, saved source, live board view; skins
  IDE dark, IDE light, Green phosphor, Amber, Lab panel.
- `tools/zforth.py`, a command-line client.
