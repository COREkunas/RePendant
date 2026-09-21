# Physical pairing shortcut — firmware 0.4.60

Historical qualification below is for 0.4.60. Newer source includes the direct
phone USB setup path; see [USB setup and key migration](PHONE_SETUP.md). The
physical five-tap gesture and its recording/power guards remain unchanged.

While the pendant is idle, press and release its button **five times within
3.5 seconds**, leaving less than half a second between taps. Do not hold it.
The pendant removes its previous Bluetooth phone bond and opens a **60-second
pairing window**, indicated by a blue blink (250 ms on / 250 ms off).

Recording data, recording/recovery keys and saved light/power settings are not
deleted. The update itself does not remove any pairing. Bond replacement happens
only after the physical gesture and successful idle/power admission. It is
refused during recording, storage work, another pairing window or unsafe power.

## Pair a phone

1. If pairing the same phone again, disconnect in OpenPendant and **Forget** its
   old pendant bond in Android Bluetooth settings. Scan and select it again.
2. With the pendant idle, perform five quick taps and check for blue blinking.
3. Start Android pairing. Read the freshly generated six-digit code locally
   using pendant USB `pairing status`, and enter it in Android's code-entry dialog.
4. Connect in OpenPendant. A different phone also needs the recording recovery
   backup to decrypt existing recordings. Do not share that private backup.

This shortcut does **not** yet make initial pairing USB-free: there is no screen
or other implemented secure passkey-delivery channel on the pendant. Bluetooth
LE Secure Connections with a 16-byte key and authenticated passkey entry remains
required. There is no fixed code, Just Works fallback or remote unpair command.

Successful pairing closes the window early. Timeout closes it after 60 seconds;
the old bond remains removed. Another five-tap gesture opens a new window.
Normal saved light behavior resumes when pairing closes. On ambiguous Bluetooth
metadata deletion, enrollment fails closed for that boot, without automatic retry.
`pairing replacement` reads the last physical replacement result without changes.

## Recording controls

A single short tap still starts/stops the same long-recording path, including
battery operation after enrollment and safe battery admission. It now waits
**600 ms of quiet** after release to distinguish one tap from five. Partial
two-to-four-tap sequences and extra taps following five are consumed, not turned
into recording commands. Five taps during recording do not stop it or re-pair.

## Implementation and evidence

- Existing GPIO, RGB PWM, standby wake, signed app-update and Zephyr bond APIs
  are reused. No factory executable fragment was transplanted. Factory button
  wake initialization was inspected at `0x1b5d0` (log literal `0x1b658`) in the
  preserved app SHA-256
  `9cc79cefe4a372e5d4ca267f3ad29fffd12c1a0fe740295a239392a272603b04`.
  No factory five-tap gesture was established.
- `src/button_gesture.h` is a pure bounded decoder. `recording_control.c` binds
  gestures to the operation present at first press. `ble_security.c` uses native
  `bt_unpair` and verifies key-entry absence without reading secret values.
- Offline production-C security/gesture tests: 242 assertions. Portable
  control/broker: 22 groups / 37,162 assertions. Development runtime integration:
  11 groups / 9,553 assertions; preference policy/persistence: 3,085 / 56 checks.
- Android 0.6.52: 634 JVM tests passed, lint 0 errors / 14 existing warnings.
  Adds pairing help and admits the unchanged firmware-60 telemetry layout.
- Firmware was installed on the owner's pendant via one signed app-only update;
  bootloader, network image and provisioning unchanged. Cached startup checks
  passed, microphone off, one existing bond retained, zero NAND I/O after boot.

Physical five-tap/LED observation, real replacement pairing and reboot/reconnect
durability are **not yet qualified**. Host fakes do not prove radio behavior,
flash durability or electrical timing. Android 0.6.52 was installed after the
phone reconnected, with the original signer and without uninstalling or clearing
app data. Real replacement pairing remains pending.
