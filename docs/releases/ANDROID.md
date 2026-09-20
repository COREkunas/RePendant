# OpenPendant Android 0.6.49 — developer preview

Companion app for the RePendant custom firmware. The installed app name is
OpenPendant, package `org.openpendant.app`, versionCode 67.

Requirements: Android 8+ / ARM64 / Bluetooth LE. The APK is development-signed
and debuggable; this is not a Play Store production release. Install manually
after verifying its SHA-256. A differently signed existing app cannot be updated
in place. Do not uninstall it without a supported recording/key migration.

Included workflows: Dashboard, expandable/filterable recordings, local playback,
selective/bulk phone/pendant deletion, transfer preferences, storage quick clear,
and Lights & battery settings. This version fixes Save waiting behind status
reads, exact acknowledgement/readback, visible errors and retained drafts.

626 JVM tests and lint (0 errors, 14 existing warnings) passed in the development
workspace. The actual Save UI was checked on USB; latest-UI battery-only
qualification remains separate. Bluetooth service-discovery stress issues remain.

No Internet or phone-microphone permission. The app uses the pendant microphone.
Whisper CPU short-clip transcription supports Lithuanian with a separately
imported multilingual base model; long-recording transcription is not integrated.
Model weights are not included. New pendants still need engineering provisioning.

APK SHA-256:
`0c2acdb55166b923e5fae2f5ee06ac41329b37b8690cfe81b4bb60ea29196f35`

Signing certificate SHA-256:
`e397851d62ace351a0ea24fa9c889ffb4811f1a4c79f2d150e2656c21c3b7632`

Keep the notices archive with redistributed APK copies. Keep your recording
recovery backup private; it is not part of a firmware or source release.
