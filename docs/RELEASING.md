# Release policy

Use independent tags for firmware (`firmware-v0.4.57`), Android
(`android-v0.6.49`) and factory research (`factory-app-v1.1.20-research`). Mark
this initial set **prerelease**. Do not present the vendor image as a custom or
fully restorable firmware release.

Before publishing:

1. Commit only the allowlisted source tree. Fetchable third-party source trees,
   local paths, build output, private keys and device data remain ignored.
2. Verify app/radio signing identities, flash address bounds and update domains.
3. Keep binaries from the tested immutable snapshots; don't silently rebuild or
   re-sign an existing release. Include SHA-256 checksums and third-party notices.
4. Describe what was physically tested and what is only an offline check.
5. Preserve every old release and original PC backup. Never replace release
   assets in place: use a new version for corrections.
6. Read the first-install warning. Do not publish a combined-image command as a
   tested turnkey installer until it has actually been qualified.

Original firmware publications must exclude NVS, UICR, external NAND and trailing
flash state. Publish only bounded, identified executable images with provenance;
do not infer permission to expose someone else's recovery secrets or recordings.

## Initial assets

- Firmware: `RePendant-firmware-0.4.57.zip` and checksums.
- Android: `OpenPendant-0.6.49.apk`, `RePendant-Android-0.6.49-notices.zip`, checksums.
- Factory research: `Limitless-factory-app-1.1.20-research.zip`, checksums.

The factory release is independent of the project's Apache license. No vendor
private key, network-core restore image or complete-restoration promise is supplied.
