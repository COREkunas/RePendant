# Hardware reference

Verified development board: Limitless Pendant, nRF5340 application/network cores,
QKAA/aQFN94 identification. Do not infer another PCB's wiring from package size.

| Function | Connection | Status |
| --- | --- | --- |
| Button | P1.05, active low | Tested; short taps, not long holds |
| RGB LED | R P0.26, G P0.27, B P0.25 | Tested; PWM period 500 μs |
| PDM microphone | CLK P0.04, DIN P0.05, enable P1.01 high | Mono recording/playback tested |
| I²C | SDA P1.06, SCL P1.14 | Gauge/identity access |
| Battery gauge | BQ27427, I²C 0x55 | Monitoring and guarded power admission |
| IMU | I²C 0x6A, WHO_AM_I 0x6A | LSM6DSM/DSL-family evidence; exact variant and motion capture not qualified |
| NAND SPI | SCK P1.08, MOSI P1.09, MISO P1.10, CS P1.15, auxiliary P1.07 high | Native-geometry recording backend |
| NAND capacity | Micron ID 2C35, 1.8 V, 4 Gbit / 512 MiB physical | Current recording layout enables 170 MiB |
| Other microphone | I²S candidates P0.19/P0.20/P0.21 | Factory-reference evidence only; not a validated recording path |

See [the SWD pad photo and voltage precautions](FIRST_FLASH.md) for programming.
Board definitions are under `independent_firmware/boards/openpendant/open_pendant`.

## Architecture

```text
PDM mic → bounded frame queue → fixed-point Opus → encrypted segments → Dhara/NAND
                                                                          │
                    Android local library ← verified BLE transfer ←───────┘
                       │                 │
                    playback       short-clip whisper.cpp
```

The phone holds its recording recovery key; public recipient enrollment binds
pendant storage. Pairing, recording state, transfer receipts and deletion are
explicit protocols, not a shared filesystem. The original factory application
was a hardware/data-flow reference; its binary is not linked into this firmware.
