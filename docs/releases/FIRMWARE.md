# RePendant firmware 0.4.57 — developer preview

Custom, recreated OpenPendant firmware for the Limitless Pendant/nRF5340.
Original project code is free to use and modify under Apache-2.0; preserve
third-party notices. This package contains verified existing app/radio updates
and offline-assembled SWD components. It is **not a turnkey stock conversion**.

- `app-update.bin`: application-only signed USB update, version 0.4.57.
- `network-update.bin`: separately signed/wrapped radio update; USB destination 3.
- `swd-application.hex`: recovery bootloader + signed application; no NVS/UICR.
- `swd-network.hex`: network boot/provision + signed controller; no UICR.
- `*-public.pem`: public update-verification keys, **not private signing keys**.
- `manifest.json`, `SHA256SUMS.txt`: bounds, verification scope and file hashes.

Read the included FIRST_FLASH.md and the repository installation guide. The SWD
files were composed and checked offline, not freshly installed on another unit.
Programming order/reset behavior matters on this dual-core chip. Factory debug
unlock can destroy original firmware, and a complete stock restore is unproven.

Long recording, encrypted storage, button/battery control, battery-qualified BLE
sync and settings are implemented. Enabled recording storage is 170 MiB of
512 MiB physical NAND. Fresh-device storage enrollment remains an engineering
workflow. Known Bluetooth stress failures remain. This is a prerelease.

The prebuilt bootloaders trust the accompanying maintainer public keys. For your
own modified firmware, build matching bootloaders using your own private keys
and install them over SWD. No signing secrets, phone keys or recording data are
included. Single-slot USB updates provide no automatic rollback.
