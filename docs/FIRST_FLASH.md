# First flash: NXP MCU-Link and the pendant SWD pads

**Audience: embedded developers comfortable with fine-pitch wiring and nRF5340
dual-core recovery. This is a wiring guide and verified historical sequence, not
an unattended installer.** No new device was flashed while preparing this release.

## 1. Before opening or erasing

Keep any available backup private. The project's factory backup includes an
application image, but **not the original network-core firmware**. Returning to
stock is unproven. Readout protection can require a destructive recover operation;
this is not a read-only unlock. Keep an SWD recovery route available.

Use an MCU-Link Base in CMSIS-DAP mode, its **target J6** connector, a 10-pin
1.27 mm ribbon cable/breakout, fine wires, a multimeter, and two 1 kΩ resistors.
Leave J3 (probe update), J4 (VCOM disable) and J5 (SWD disable) open for the
verified setup. Do not connect to the probe's own-development SWD connector.
See [NXP UM11931](https://www.nxp.com/docs/en/user-manual/UM11931.pdf), especially
the board table and target voltage/power-order discussion.

## 2. Pads and wiring

![User-supplied pad photograph; SWDIO was subsequently validated by SWD responses](images/pendant-swd-pads.png)

The photo is the owner's actual opened board, preserved without retouching.
It is not a universal PCB-revision drawing. The SWDIO candidate shown here was
subsequently validated through repeated SWD DPIDR replies (`0x6BA02477`) at
125 kHz. A pad measuring 1.8 V alone does **not** prove it is SWDIO.

| Pendant pad | MCU-Link 10-pin target connector | Verified connection |
| --- | --- | --- |
| GND | **3 — GND** | Direct |
| 1.8 V | **1 — Vref** | Direct; voltage reference, not a 3.3 V supply |
| SWDIO | **2 — SWDIO / SWIO** | Through 1 kΩ |
| SWDCLK / SWCLK | **4 — SWCLK / CLK** | Through 1 kΩ |
| RESET | 10 — nRESET | **Left disconnected** in the verified setup |
| Unlabelled neighbouring pad | None | Leave disconnected |

Use connector pin numbering and a continuity check, not just ribbon orientation.
Do not use pin 9/ground-detect as the sole ground connection. The unlabelled pad
below SWCLK is not a verified SWO connection.

1. Remove USB and battery power before soldering or changing wires. Avoid battery
   shorts; do not work on a damaged or swollen battery.
2. With everything unpowered, verify ground and cable continuity.
3. Power MCU-Link over USB **before powering the target**; NXP warns that an
   unpowered probe can be back-powered by the target.
4. Power the pendant from its battery. Measure **Vref-to-GND ≈ 1.8 V** at the
   connected breakout. If it is 3.3 V, collapses or is unstable, stop.
5. Begin at **125 kHz SWD**. A successful debug-port read proves only the SWD
   connection, not flash access, correct firmware or permission to erase.

Never feed the pendant's 1.8 V rail with a 3.3 V output. MCU-Link uses target
reference tracking; do not treat an unloaded voltage reading as a power source.

## 3. Initial programming sequence

The working installation used these stages:

1. Verify the exact nRF5340 target and both access ports. Preserve what can be read.
2. If protected, explicitly recover the **network core, then application core**.
   Recovering after programming can erase your work. Confirm development debug
   access instead of blindly overwriting UICR. This step destroys stock contents.
3. Program and read back the **network boot/provision + signed radio** first,
   without resetting the application between network access setup and programming.
4. Program and verify the **application bootloader + signed application**. A
   controlled application soft reset was needed to clear temporary SPU locks.
5. Read back and compare both image regions. Verify USB application and MCUboot
   recovery enumeration before removing SWD access.
6. Disconnect power before removing SWD wires; then test independent USB operation.

The initial attempt to program a combined image using probe-rs 0.32.0 failed at
network reset/halt **after** programming the application. A generic “flash this
combined HEX” command would therefore misrepresent the proven process. The
successful network stage used a guarded pyOCD 0.42 helper and Nordic DFP 8.44.1
with corrected network sector metadata. The unmodified pack/pyOCD combination
had a zero-sector-size problem. Those device-specific laboratory helpers are
not a qualified public installer.

The release contains separate `swd-network.hex` and `swd-application.hex` to make
the domains explicit. Their address ranges exclude application NVS and UICR.
They are offline-assembled from verified build artifacts; a fresh-device install
of this assembled package has **not** been qualified. **Stop here if you need a
turnkey installation; do not improvise mass erase/retry sequences.**

## 4. USB updates after custom recovery exists

Stop recording and syncing, use stable USB power, close other serial owners, and
confirm your bootloader trusts the release's public signing key. In the custom
application's USB shell, `pendant status` is a status check. The explicit command
`pendant recovery confirm` enters MCUboot recovery and re-enumerates USB.

Use an MCUboot serial/SMP uploader with the **signed app-update BIN**, confirm its
reported image digest/version, then restart and check health. The radio wrapper
is a separate **network destination 3** operation, not application destination 0
and not a raw network flash image. Do not update the radio routinely.

This bootloader is **single-slot: no automatic rollback**. If an update outcome
is uncertain, inspect recovery status before another upload; do not blindly retry
or disconnect during network copying. The repository does not yet ship a generic
qualified USB updater.

## 5. New owner and storage setup

Installing code does not copy the maintainer's pairing or encrypted recording
volume. Create your own phone recording key and verify its recovery backup;
pair through an explicitly opened enrollment window. A new pendant also needs
device-bound storage provisioning and bad-block validation. That workflow still
uses engineering tools and is not a one-tap APK setup in this release.

Never reuse someone else's NVS, UICR, recovery key or NAND descriptor. If you build
modified firmware, generate your own signing keys **before first installation**
and build the matching bootloaders. The prebuilt recovery trusts the maintainer's
key; unsigned or differently signed forks cannot be uploaded through it.

References: [Nordic nRF5340 debug](https://docs.nordicsemi.com/r/bundle/ps_nrf5340/page/debugandtrace.html),
[CTRL-AP](https://docs.nordicsemi.com/r/bundle/ps_nrf5340/page/ctrl-ap.html),
[SPU](https://docs.nordicsemi.com/r/bundle/ps_nrf5340/page/spu.html).
