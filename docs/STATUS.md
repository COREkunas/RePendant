# Development status — 2026-09-20

This is an engineering snapshot, not a claim that every workflow is production
ready. Results below refer to the owner's test pendant, not all PCB revisions.

| Area | Evidence / current boundary |
| --- | --- |
| Firmware | 0.4.57-battery-settings installed and checked |
| Android | 0.6.49, versionCode 67; 626 JVM tests passed in the development workspace; lint 0 errors / 14 existing warnings |
| Microphone | Intelligible mono PDM audio; five-minute real-mic recording/save/phone-transfer tests, including a battery interval |
| Button | Start/stop without per-recording app approval after enrollment; standby wake and battery recording tested |
| Battery sync | Firmware 0.4.57 / app 0.6.48 battery-only settings and storage-sync/reconnect checks passed |
| Latest Save UI | App 0.6.49 fixes status-read/save races, exact acknowledgement/readback and draft retention; actual UI tested on USB, not requalified battery-only |
| Storage | 512 MiB physical NAND; 170 MiB enabled recording space; encrypted segmented objects, receipts and deletion |
| Power | Low-power standby, not full power-off; no measured all-day runtime claim |
| Bluetooth | Improved delivery and recovery, but a broader service-discovery stress failure remains; no blanket reliability or original-speed parity claim |
| Transcription | CPU whisper.cpp, multilingual base model including Lithuanian; short-clip path only; model not bundled |
| Hardware | Button, RGB, PDM mic, NAND and battery gauge exercised; IMU identity read, not continuous motion capture; second/I2S microphone not qualified |
| Fresh installation | One laboratory SWD install succeeded through a staged procedure; portable installer and new-owner provisioning remain unfinished |
| Factory restore | Not demonstrated; original network-core image missing |

The release APK and signed update are the preserved tested binaries. Public build
configuration is adapted for independent developers; reproducible clean-build
qualification and second-device testing are separate work, not implied by copying
these binaries. No device was erased, flashed or recorded for publication.

## Next priorities

1. Qualified, fail-closed first-install and new-volume enrollment tooling.
2. Bluetooth service-discovery/reconnection stress coverage across phones.
3. Battery-only latest-UI qualification and repeatable power measurements.
4. Long-recording transcription and model lifecycle improvements.
5. Safely expand usable storage beyond the current 170 MiB layout.
