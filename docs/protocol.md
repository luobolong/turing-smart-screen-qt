# 35inchNEW / Rev A serial protocol

English | [简体中文](protocol.zh-CN.md)

Applies to 320×480 portrait and 480×320 landscape displays; hardware and firmware revisions may differ. Byte sequences are hexadecimal, coordinates and sizes are decimal. B0 is the first byte of a message.

## How it works

The computer collects sensor data, renders images, and sends messages over the serial port. The display uses the command in each message to change its settings or update its picture.

Every message has a **6-byte header** containing an operation code and four parameter fields. Commands that need additional data append a **payload**. The operation code and its parameters determine whether a payload exists and how long it is:

```text
Commands
├── Without payload: the entire message is a 6-byte header
│   ├── Zero parameters: reset, clear, fill black, screen on/off, probe
│   └── With parameters: brightness (value stored in the header)
└── With payload: 6-byte header + additional data
    ├── Control data: orientation, mirroring
    └── Image data: RGB bitmap, extended image formats
```

## Common header

All commands use the same encoding. **Field meanings depend on the command; field positions and widths do not change.**

| Bytes | Contents |
| --- | --- |
| B0–B4 | Four packed 10-bit parameter fields: `x`, `y`, `end_x`, `end_y`, in that order. |
| B5 | One-byte operation code, `command`. |

The field names come from drawing: bitmap commands use them for top-left and bottom-right coordinates; brightness reuses `x` for its value; unused fields are zero. A command **without a payload can still have parameters**.

Each parameter field encodes 0–1023. This reference uses `header(x, y, end_x, end_y, command)` for the following packing rule:

```text
packed = (x << 30) | (y << 20) | (end_x << 10) | end_y
B0..B4 = packed in big-endian order (most significant byte first)
B5     = command
```

The packed value is 40 bits. In C/C++, cast fields to a 64-bit unsigned type before shifting. The payload is not part of this packing; its format is defined by each command.

## Commands without payload

The message ends at B5 and is always **6 bytes** long.

### Zero-parameter commands

Common format: `header(0, 0, 0, 0, command)`.

| Code | Operation |
| --- | --- |
| `0x65` | Reset. |
| `0x66` | Clear screen. |
| `0x67` | Fill with black. |
| `0x6C` | Screen off. |
| `0x6D` | Screen on. |
| `0xFF` | Connection probe; the reply determines firmware timing. |

Probe message: `00 00 00 00 00 FF`. For clear-screen compatibility, this project sets orientation to 0, sends `0x66`, then restores the previous orientation.

### Parameterized command: brightness — 0x6E

Format: `header(raw, 0, 0, 0, 0x6E)`. The raw value ranges from 0 to 255: 0 is brightest, 255 darkest. Percentage `p` (0–100) converts as follows:

```text
raw = floor(255 × (100 - p) / 100)
```

Example: 75% → raw 63 → `0F C0 00 00 00 6E`. The brightness value is already in the header; no data follows it.

## Commands with payload

The payload starts at **B6**, after the header. Total length is `6 + payload bytes`. All offsets below are measured from the start of the entire message.

### Control data

Orientation and mirroring both use a header with zero parameters followed by a **10-byte payload**, for a total of **16 bytes**.

#### Orientation — 0x79

Header: `header(0, 0, 0, 0, 0x79)`.

| Payload bytes | Contents |
| --- | --- |
| B6 | Decimal `100 + direction` |
| B7–B8 | Width, 16-bit big-endian |
| B9–B10 | Height, 16-bit big-endian |
| B11–B15 | Zeros |

| direction | Orientation | Width × height | B6 |
| --- | --- | --- | --- |
| 0 | Portrait | 320×480 | `64` |
| 1 | Reverse portrait | 320×480 | `65` |
| 2 | Landscape | 480×320 | `66` |
| 3 | Reverse landscape | 480×320 | `67` |

Complete landscape message: `00 00 00 00 00 79 66 01 E0 01 40 00 00 00 00 00`.

#### Mirroring — 0x7A

Header: `header(0, 0, 0, 0, 0x7A)`. Payload byte B6 is `00` (off) or `01` (on); B7–B15 are zeros.

### Image data

Image commands specify the drawing location in the header and supply colors or pixels in the payload. Byte order is defined separately for each format below.

#### RGB bitmap — 0xC5

Used for full-screen and partial updates. The origin is top-left; x increases rightward, y downward. The rectangle includes its bottom-right endpoint and must fit the current display dimensions.

```text
Header:  header(x, y, x + width - 1, y + height - 1, 0xC5)
Payload: width × height × 2 bytes
Order:   rows top to bottom, pixels left to right
```

Each pixel is RGB565 (5 red, 6 green, 5 blue bits), sent **little-endian** (low byte first). There is no file header, alpha channel, or row padding. Conversion from 8-bit RGB channels:

```text
rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
bytes  = [rgb565 & 0xFF, rgb565 >> 8]
```

| Color | RGB565 | Bytes |
| --- | --- | --- |
| Red | `F800` | `00 F8` |
| Green | `07E0` | `E0 07` |
| Blue | `001F` | `1F 00` |
| White | `FFFF` | `FF FF` |
| Black | `0000` | `00 00` |

Full-screen payload: **307200 bytes**; total message: **307206 bytes**.

| Dimensions | Rectangle (inclusive) | Header |
| --- | --- | --- |
| 320×480 | (0, 0)–(319, 479) | `00 00 04 FD DF C5` |
| 480×320 | (0, 0)–(479, 319) | `00 00 07 7D 3F C5` |

Example: a red 2×1 rectangle at (10, 20), ending at (11, 20). The first 6 bytes are the header; the last 4 bytes are two red pixels:

```text
02 81 40 2C 14 C5 00 F8 00 F8
```

#### Extension: single-color point list — 0xC3

Known layout, **64 bytes total** (6-byte header + 58-byte payload); support across Rev A firmware is not guaranteed.

```text
B0..B5 = header(base_x, base_y, coord_byte_count, 0, 0xC3)
B6..B7 = RGB565 color, big-endian (high byte first)
B8..   = dx0, dy0, dx1, dy1, ...
```

`coord_byte_count` counts valid coordinate bytes: even, at most 56 (28 points). Each component is 1 byte; trailing bytes are not coordinates. Coordinates are relative to the base point; actual firmware drawing behavior remains unconfirmed.

#### Extension: monochrome bitmap — 0xC6

Known layout; support across Rev A firmware is not guaranteed. The rectangle header is the same as `0xC5`, with command `0xC6`. The payload packs pixels sequentially at 1 bit per pixel, most significant bit first within each byte.

Known sizes: 68×90 → 765 payload bytes; 48×66 → 396 payload bytes. There is no color field; bit-to-color mapping and background handling remain unconfirmed.

## Transmission rules

- **Serial settings:** 115200 baud, 8N1 (8 data bits, no parity, 1 stop bit). DTR/RTS asserted; hardware and software flow control disabled.
- **Message boundaries:** no start/end marker, separate length field, sequence number, or CRC. USB packet boundaries do not delimit messages.
- **Ordering:** a header and its payload must not be interleaved with other commands. Normal bitmap transfers do not wait for a per-frame ACK.

A probe response containing ASCII `V2` selects V2 timing; otherwise use V1:

| After sending | V1 wait | V2 wait |
| --- | --- | --- |
| Orientation / mirroring command | 10 ms | 10 ms |
| RGB bitmap header, before pixel data | 10 ms | None |
| RGB bitmap payload, before next command | 10 ms | None |

Implementation: [src/device.cpp](../src/device.cpp). Known-byte tests: [protocol()](../tests/core_test.cpp).
