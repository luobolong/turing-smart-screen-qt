# 35inchNEW / Rev A serial protocol

English | [简体中文](protocol.zh-CN.md)

Supported dimensions: 320×480 in portrait orientation and 480×320 in landscape orientation. Hardware and firmware revisions may differ. Byte examples below are hexadecimal, and byte offsets start at 0.

## Serial connection and messages

- Baud rate: 115200; data format: 8N1.
- Assert DTR and RTS; disable hardware and software flow control.
- Each message consists of a **6-byte command header + optional payload**. Its length depends on the command.
- The header has no fixed start marker, length field, sequence number, or CRC. Normal bitmap transfers do not wait for a per-frame ACK.
- Send each message's header and payload consecutively, without inserting other commands. USB packet boundaries are not application-level message boundaries.

## Command header

The first 5 bytes pack four 10-bit fields in order. The last byte is the command:

```text
packed = (x << 30) | (y << 20) | (end_x << 10) | end_y
B0..B4 = packed as 5 bytes in big-endian order
B5     = command
```

| Byte | Contents |
| --- | --- |
| B0 | x[9:2] |
| B1 | Upper 2 bits: x[1:0]; lower 6 bits: y[9:4] |
| B2 | Upper 4 bits: y[3:0]; lower 4 bits: end_x[9:6] |
| B3 | Upper 6 bits: end_x[5:0]; lower 2 bits: end_y[9:8] |
| B4 | end_y[7:0] |
| B5 | Command |

Each field can encode values from 0 to 1023. Drawing coordinates must also be within the current screen dimensions. Control commands may reuse the coordinate fields for parameters.

## Control commands

| Command | Total length | Meaning and parameters |
| --- | --- | --- |
| `0x65` | 6 bytes | Reset; all four fields are 0. |
| `0x66` | 6 bytes | Clear the screen; all four fields are 0. For compatibility, set orientation to 0 before clearing, then restore the previous orientation. |
| `0x67` | 6 bytes | Fill with black; all four fields are 0. |
| `0x6C` | 6 bytes | Turn the screen off; all four fields are 0. |
| `0x6D` | 6 bytes | Turn the screen on; all four fields are 0. |
| `0x6E` | 6 bytes | Set brightness; put the raw brightness value in x and set the other fields to 0. |
| `0x79` | 16 bytes | Set orientation and dimensions; see below. |
| `0x7A` | 16 bytes | Set mirroring; see below. |
| `0xFF` | 6 bytes | Connection probe: `00 00 00 00 00 FF`. |

### Brightness

A raw value of `0` is brightest; `255` is darkest. Convert a percentage `p` (0–100) as follows:

```text
raw = floor(255 × (100 - p) / 100)
```

Brightest: `00 00 00 00 00 6E`; darkest: `3F C0 00 00 00 6E`. Brightness is packed into the x field, so it cannot be written directly into the first byte.

### Orientation

| Offset | Contents |
| --- | --- |
| 0–4 | All zeros |
| 5 | `79` |
| 6 | Decimal `100 + direction` |
| 7–8 | Width, 16-bit big-endian |
| 9–10 | Height, 16-bit big-endian |
| 11–15 | All zeros |

| direction | Orientation | Width × height | B6 |
| --- | --- | --- | --- |
| 0 | Portrait | 320×480 | `64` |
| 1 | Reverse portrait | 320×480 | `65` |
| 2 | Landscape | 480×320 | `66` |
| 3 | Reverse landscape | 480×320 | `67` |

Landscape example:

```text
00 00 00 00 00 79 66 01 E0 01 40 00 00 00 00 00
```

### Mirroring

The first 5 bytes are 0, B5 is `7A`, B6 is `00` (off) or `01` (on), and B7–B15 are zero-filled. Example with mirroring enabled:

```text
00 00 00 00 00 7A 01 00 00 00 00 00 00 00 00 00
```

## RGB bitmap: 0xC5

The header specifies a rectangle with an inclusive bottom-right endpoint, followed by tightly packed RGB565 pixels:

```text
Header: header(x, y, x + width - 1, y + height - 1, 0xC5)
Payload length: width × height × 2 bytes
Pixel order: rows from top to bottom, each row from left to right
```

Send each pixel in **little-endian** order, without an image file header, alpha channel, or row padding. Convert 8-bit RGB channels to RGB565 as follows:

```text
rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
Send order: rgb565 & 0xFF, then rgb565 >> 8
```

| Color | RGB565 | Bytes sent |
| --- | --- | --- |
| Red | `F800` | `00 F8` |
| Green | `07E0` | `E0 07` |
| Blue | `001F` | `1F 00` |
| White | `FFFF` | `FF FF` |
| Black | `0000` | `00 00` |

A full-screen update has a 307200-byte payload in either orientation:

| Dimensions | Top-left → bottom-right | Header |
| --- | --- | --- |
| 320×480 | (0, 0) → (319, 479) | `00 00 04 FD DF C5` |
| 480×320 | (0, 0) → (479, 319) | `00 00 07 7D 3F C5` |

Complete message for drawing two red pixels horizontally, starting at (10, 20):

```text
02 81 40 2C 14 C5 00 F8 00 F8
```

Partial updates also use `0xC5`; only the rectangle coordinates and corresponding payload length change.

## Transmission timing

After connecting, you can send the probe, orientation, mirroring, and brightness commands in that order. Wait 10 ms after each orientation or mirroring command.

- **V1**: Wait 10 ms after sending the bitmap header before sending the pixels. Wait another 10 ms after the pixels have been sent.
- **V2**: Omit both bitmap-transfer waits described above. Keep the waits after orientation and mirroring commands.

Use V2 timing if the probe response contains ASCII `V2`. If the version cannot be determined, use V1 timing.

## Extended commands

The following sections describe known message layouts only. Do not assume that all Rev A firmware supports them.

### Single-color point list: 0xC3

The message is always 64 bytes long:

```text
B0..B5 = header(base_x, base_y, coord_byte_count, 0, 0xC3)
B6..B7 = RGB565 color, high byte first
B8..   = dx0, dy0, dx1, dy1, ...
```

`coord_byte_count` is the number of valid coordinate bytes. It must be even and at most 56 bytes, representing up to 28 points. Each coordinate component occupies 1 byte; any remaining trailing bytes are not valid coordinates.

This command uses **big-endian** color, unlike `0xC5`. Coordinates are interpreted relative to the base point, but the firmware's actual point-drawing behavior still needs verification.

### Monochrome bitmap: 0xC6

The rectangle header has the same format as `0xC5`, with the command changed to `0xC6`. Pixels are packed sequentially at 1 bit per pixel, most significant bit first within each byte.

Known dimensions include 68×90 (765 bytes) and 48×66 (396 bytes). The message carries no color. The firmware determines which colors the bit values represent and how the background is handled; this behavior has not been confirmed. Do not use RGB565 data as the payload for this command.
