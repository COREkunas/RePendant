# Public export checks — 2026-09-24

## Source update: firmware 0.4.63 / Android 0.6.62

- Exported the owner's existing development updates, matching **180 firmware,
  153 Android main and 87 JVM-test files** to the preserved release snapshots
  (full Android baseline plus later incremental overlays; text line endings
  normalized). Vendor source, private instrumentation, device logs, recordings,
  accounts and signing/recovery keys remain excluded.
- Re-ran public Android unit tests and lint: **731 tests, zero failures, errors
  or skips**; lint **0 errors / 35 warnings**. Build completed successfully.
  This used the public developer-signing configuration without installing an APK.
- Public native tests passed: **3,085 preference checks + 118 runtime/persistence
  checks**, **1,680,080 battery assertions / 423 groups**, **253 BLE security
  assertions**, and **37,162 recording-control assertions / 22 groups**.
- Preserved public signing/build portability. The new preferences test uses the
  existing public compiler helper instead of importing the private runtime/Dhara
  harness; production sources and test expectations are unchanged.
- Updated privacy and licensing notices for explicit optional MindyLink/Forge
  networking, OkHttp, test-only JSON-java and Lucide/Feather icons. Retained the
  separate MindyLink licence; no blanket Apache relicensing of derived portions.
- Publication checks cover tracked paths, credential/device-identifier patterns,
  relative documentation links and source-snapshot equality. This is a targeted
  check, not a comprehensive security audit.

No hardware connection, firmware rebuild/flash, microphone capture, account use,
audio upload, recording deletion or new binary release was performed for this
source update. [Feature details and physical-test limits](SEPTEMBER_2026_UPDATES.md)
distinguish existing development evidence from this offline publication check.
Existing downloads/tags remain firmware 0.4.57 / Android 0.6.49.

## Historical source update: firmware 0.4.62 / Android 0.6.56

Includes the previously unexported USB pairing, phone migration and new-key reset
sources plus metadata-only recording sync. Private keys, factory backups, phone
identifiers, device-specific operators and recording data remain excluded.

- Development Android build: **682 JVM tests, zero failures/errors/skips**;
  lint **0 errors / 34 warnings**. No integrity tests disabled.
- Independently re-ran the exported Android source: **682 tests passed**;
  lint **0 errors / 34 warnings**. Compared all **180 firmware, 118 Android main
  and 81 JVM-test files** against the preserved release sources, permitting only
  line-ending normalization. Public signing/build adaptations remain intact.
- Audited **503 tracked source/document files** for private/generated paths,
  credential/device-identifier patterns and valid local document links. This is
  a targeted publication check, not a universal secret-detection guarantee.
- Installed Android 0.6.56 with its verified original signer, preserving app data.
  Production-controller details-only sync passed on the enrolled phone/pendant:
  **one catalog request, zero audio reads/bytes, zero receipts/deletions**.
  The physical catalog was empty; populated/paginated behavior was tested offline.
- Exported production-C pairing/security tests: **253 assertions passed**;
  recording-control/broker tests: **22 groups / 37,162 assertions passed**.
- Firmware 0.4.62 is the preserved previously installed snapshot, not a new flash
  in this task. Its earlier reset qualification includes **7,992 configuration
  assertions** and **24 native-volume test groups** including interrupted formats.
- Metadata-only work performed no recording, playback, NAND erase, key rotation
  or firmware update. Prior reset qualification is described in
  [phone setup and migration](PHONE_SETUP.md), including its physical limits.

Published release downloads/tags remain firmware 0.4.57 / Android 0.6.49; this is
a newer source commit, not a replacement binary release.

## Historical source update: firmware 0.4.60 / Android 0.6.52

The current export includes the battery-recovery changes and the physical
five-tap pairing shortcut. Application sources match the final tested snapshots
apart from text line endings; public signing/build portability stays intact.

- Development Android build: **634 JVM tests, 0 failures, 0 errors**;
  lint **0 errors / 14 existing warnings**. The initial new-version runs exposed
  two fixtures still declaring firmware 60 unknown; they now accept 60 and
  continue rejecting 61. No tests were removed or disabled.
- Re-ran exported production-C security/gesture tests: **242 assertions**;
  portable recording-control/broker: **22 groups / 37,162 assertions**.
- Additional development-only runtime integration: **11 groups / 9,553
  assertions**; preference policy/persistence: **3,085 / 56 checks**.
- Application signature, ELF/payload identity, partition bounds, source/object
  freshness and fixed RAM growth audited; radio/boot/provisioning bytes unchanged.
- One signed application-only update installed on the owner's pendant. Cached
  startup passed with microphone off, one old bond retained, valid battery
  samples and zero NAND I/O after boot. No pairing or recording was triggered.

Physical gesture/LED and new-bond durability remain unqualified. Android 0.6.52
was installed with the verified original signer and without clearing app data.
Existing downloadable release binaries
and tags remain at firmware 0.4.57 / Android 0.6.49. See
[pairing details](BUTTON_PAIRING.md) and [battery limits](GAUGE_RECOVERY.md).

## Initial release export (historical)

Completed without accessing, flashing or recording from the pendant or phone:

- Compared all 179 archived firmware source files, 106 archived Android main
  files and 76 archived JVM-test files against the export: application source
  unchanged, apart from permissible text line-ending normalization.
- Adapted only public build setup: explicit new-developer Android signing opt-in
  and SDK-relative Dhara verification. Private signing files are not exported.
- Re-ran the exported Android JVM suite: **626 tests, 0 failures, 0 errors**.
- Re-ran lint: **0 errors, 14 existing warnings**.
- The first sandboxed test attempt failed on Windows real-path permission checks;
  the same tests passed outside that restriction. No tests were disabled.
- Verified 482 Opus files against the pinned official source archive, the
  whisper.cpp source revision/clean tree, and eight compiled Dhara source/header
  hashes.
- Verified the preserved APK signing certificate and SHA-256.
- Verified custom app and radio wrapper signatures using public keys, the inner
  radio signature, and the network provision public-key hash.
- Checked SWD HEX address bounds exclude UICR and application settings storage.
- Parsed the original application header/TLVs and validated its embedded digest
  before extracting only the bounded image. Full backup remains private.
- Checked tracked filenames, credential patterns, source identifiers, relative
  documentation links, ZIP paths and every release-member checksum.

These checks are not an independent security audit. The exported firmware was
not rebuilt or freshly installed; SWD packages were assembled offline. No new
phone/pendant end-to-end qualification is implied. The preserved release binaries
are not replaced by the test build. All original backups and device state remain
unchanged.
