# Public export checks — 2026-09-20

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
