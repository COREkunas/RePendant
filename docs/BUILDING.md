# Build from source

Use a short checkout path on Windows: nested CMake/NDK output can exceed path
limits. Build commands below do not flash a device. They describe a development
environment, not a newly qualified binary or a one-step first installation.

## Dependencies

Install Git, Python 3.11+ (3.12 recommended), nRF Connect SDK **v3.4.0** and its
matching toolchain. For Android use JDK 17+, Android SDK 35, Build Tools 34.0.0,
NDK **30.0.14904198**, and CMake **4.2.1**. Gradle 8.9 is pinned by the wrapper.

From the checkout root:

```console
python tools/fetch_dependencies.py
python tools/verify_opus_integration.py --source-only
```

This explicitly downloads the official Opus 1.6.1 source archive and the pinned
whisper.cpp v1.9.4 checkout. It verifies hashes/revision and does not overwrite
existing sources or fetch models. SDK/Gradle dependencies are separate downloads.

## Android

Create `android_app/local.properties` with your local paths (forward slashes):

```properties
sdk.dir=C:/path/to/Android/Sdk
cmake.dir=C:/path/to/cmake-install-prefix
```

`cmake.dir` is optional if Android Studio can locate the pinned version. The SDK
and NDK paths are not portable; never commit `local.properties`.

For your **first independent debug build**, from `android_app`:

```console
./gradlew -PnewDeveloperBuild=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

On Windows use `gradlew.bat`. Gradle uses your own debug signing identity. A new
key cannot update an APK signed by someone else. **Do not uninstall a data-bearing
app just to bypass a signature mismatch**; preserve its recording recovery key
and data through a supported migration first. For updates signed by your existing
standard Android debug key, set `owner.debug.keystore` to that same file in
`local.properties` instead of using the new-developer option.

Output: `app/build/outputs/apk/debug/app-debug.apk`. This is a debuggable ARM64
development build. The public source includes JVM tests; private device-specific
instrumentation and test recordings are intentionally excluded. Import a
compatible multilingual base transcription model separately through the app.

## Firmware: your own signing keys

Activate the **v3.4.0 SDK toolchain environment**. Set `ZEPHYR_BASE` to that SDK's
`zephyr` directory. Install the pinned Opus source above. Use absolute paths for
the following commands, replacing `SDK`, `REPO`, `APP_KEY`, `NETWORK_KEY` and
`BUILD_DIR` with your paths.

For a **new installation only**, generate separate signing keys in a private
directory outside the repository:

```console
python SDK/bootloader/mcuboot/scripts/imgtool.py keygen -t ecdsa-p256 -k APP_KEY
python SDK/bootloader/mcuboot/scripts/imgtool.py keygen -t ecdsa-p256 -k NETWORK_KEY
```

Do not overwrite an existing installation's signing keys. Back them up privately.
Do not substitute the SDK example key. A prebuilt recovery image does not trust
your new key; your first SWD installation must include matching bootloaders.

```console
west build -b open_pendant/nrf5340/cpuapp --sysbuild -s REPO/usb_firmware -d BUILD_DIR -- -DOPENPENDANT_RECORDING_PROFILE=ON -DOPENPENDANT_NATIVE_STORAGE=ON -DOPENPENDANT_LONG_CONTROL=ON -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=APP_KEY -DSB_CONFIG_SECURE_BOOT_SIGNING_KEY_FILE=NETWORK_KEY
```

Use a **new empty build directory**. This selects the current native-storage,
long-recording and battery-control profile; leaving these flags off selects old
diagnostic code instead. Dhara's compiled source hashes and Opus integration are
checked during the build. Do not disable these checks to make a mismatched SDK
pass. CMake receives the actual SDK Dhara location, not a maintainer PC path.

Important output domains:

| Output relative to `BUILD_DIR` | Purpose |
| --- | --- |
| `usb_firmware/zephyr/zephyr.signed.bin` | Signed MCUboot application update |
| `usb_firmware/zephyr/zephyr.signed.hex` | Application address-mapped image |
| `mcuboot/zephyr/zephyr.hex` | Application-side recovery bootloader |
| `b0n_provision_merged.hex` | Network bootloader and signing-key provisioning |
| `signed_by_b0_hci_ipc.hex` | Signed network image in its own address space |
| `signed_by_mcuboot_and_b0_hci_ipc.bin` | **Wrapped** network USB recovery update, not raw SWD input |

Before programming, independently check image signatures, address bounds,
partition configuration, UICR exclusion, memory use and no overlap with NVS.
`west flash` or programming the combined output is **not** a qualified first-flash
recipe here. Read [FIRST_FLASH.md](FIRST_FLASH.md).

The public export changes only build portability/documentation around the
preserved application sources. Rebuilding signatures is not byte-identical
reproduction of the downloaded, maintainer-signed binaries.
