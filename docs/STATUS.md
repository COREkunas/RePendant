# Development status — 2026-09-21

This is an engineering snapshot, not a claim that every workflow is production
ready. Results below refer to the owner's test pendant, not all PCB revisions.

| Area | Evidence / current boundary |
| --- | --- |
| Firmware | 0.4.62-key-reset installed; new-key pendant-only reset completed through PC-wired operator flow; empty storage survives restart; USB recovery and bond retained |
| Android | 0.6.56, versionCode 74 installed with original signer/data retained; 682 JVM tests passed; lint 0 errors / 34 warnings |
| Microphone | Intelligible mono PDM audio; five-minute real-mic recording/save/phone-transfer tests, including a battery interval |
| Button | Start/stop without per-recording app approval after enrollment; standby wake and battery recording tested |
| Five-tap pairing / phone setup | Idle-only replacement and authenticated 60-second window retained; direct phone USB setup added; owner reported new-phone pairing; complete direct-phone USB migration/reset qualification remains |
| Recording key migration | Keep-key import supported; new-key erase completed on one pendant with phone copies/backups retained; encrypted archive migration disabled/unimplemented |
| Details-only sync | Physical empty-catalog check passed: 1 catalog call, 0 audio bytes/reads, receipts or deletions; populated/paginated fixtures passed offline; separate explicit pending-deletion completion |
| Battery sync | Firmware 0.4.57 / app 0.6.48 battery-only settings and storage-sync/reconnect checks passed |
| Latest Save UI | App 0.6.49 fixes status-read/save races, exact acknowledgement/readback and draft retention; actual UI tested on USB, not requalified battery-only |
| Latest readiness/sync UI | App 0.6.51 retains battery-monitor warnings, distinguishes USB fallback from battery readiness, and identifies catalog-reading versus the last completed sync |
| Battery reconnection | Physical battery-only wait followed by USB passed deferred monitor restoration in about 114 seconds; fully reset gauge still requires USB before recording |
| Storage | 512 MiB physical NAND; 170 MiB enabled recording space; encrypted segmented objects, receipts and deletion |
| Power | Low-power standby, not full power-off; no measured all-day runtime claim |
| Bluetooth | Latest cold-boot check passed four empty-catalog syncs and automatic recovery after link loss/status/foreground transitions; broader stress and populated-transfer qualification remain, with no blanket reliability or original-speed parity claim |
| Transcription | CPU whisper.cpp, multilingual base model including Lithuanian; short-clip path only; model not bundled |
| Hardware | Button, RGB, PDM mic, NAND and battery gauge exercised; IMU identity read, not continuous motion capture; second/I2S microphone not qualified |
| Fresh installation | One laboratory SWD install succeeded through a staged procedure; portable installer and new-owner provisioning remain unfinished |
| Factory restore | Not demonstrated; original network-core image missing |

Published downloads still contain app 0.6.49 and firmware 0.4.57; this source
snapshot is newer. See [battery recovery evidence and limits](GAUGE_RECOVERY.md)
and [pairing behavior and qualification limits](BUTTON_PAIRING.md),
[phone setup/key migration](PHONE_SETUP.md) and [details-only sync](METADATA_SYNC.md).
The release APK and signed update are the preserved tested binaries. Public build
configuration is adapted for independent developers; reproducible clean-build
qualification and second-device testing are separate work, not implied by copying
these binaries. No device was erased, flashed or recorded for publication.

## Next priorities

1. Qualified, fail-closed first-install and new-volume enrollment tooling.
2. Bluetooth service-discovery/reconnection stress coverage across phones.
3. Battery-only reset-gauge recovery, latest-UI qualification and repeatable power measurements.
4. Long-recording transcription and model lifecycle improvements.
5. Safely expand usable storage beyond the current 170 MiB layout.
