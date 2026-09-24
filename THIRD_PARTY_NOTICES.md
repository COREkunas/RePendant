# Licensing and third-party components

Original RePendant/OpenPendant code and documentation: Apache License 2.0, see
[LICENSE](LICENSE). Existing file notices take precedence for third-party files.
The project permits free use, modification and redistribution subject to these
terms. This statement does not relicense vendor firmware, trademarks, models or
all components of the Nordic SDK.

| Component | Version/reference | License / notice |
| --- | --- | --- |
| Zephyr / MCUboot | nRF Connect SDK v3.4.0 dependency revisions | Apache-2.0; [MCUboot notice](LICENSES/MCUboot.txt) and SDK per-file notices |
| Nordic SDK, nrfxlib, controller, MPSL, cryptography | nRF Connect SDK v3.4.0 | [Nordic notice](LICENSES/Nordic.txt), [SDK notice](LICENSES/Nordic-SDK.txt), and component notices in `LICENSES/` |
| nrfx HAL | SDK-pinned revision | [BSD-3-Clause](LICENSES/nrfx-BSD-3-Clause.txt) and existing source notices |
| Picolibc / GCC runtime | Matching NCS toolchain | Picolibc/Newlib per-file notices and GCC runtime exception in `LICENSES/` |
| Dhara | `6f163ca05e174b168b4d148160b50eeaeeb561fc` | [Upstream license](LICENSES/Dhara.txt) |
| Opus | 1.6.1 | [COPYING](LICENSES/OPUS_COPYING.txt), BSD-style component terms |
| whisper.cpp / ggml | v1.9.4, `927cfce34f31707e17f2bff35c349632fb9e2c3a` | [MIT](LICENSES/whisper-MIT.txt); preserve upstream component notices |
| Kotlin / Gradle / Android tools | Build dependencies | Their upstream licenses; tools are not redistributed here except Gradle wrapper |
| Android C++ runtime | NDK 30.0.14904198 | LLVM/libc++ notices in `LICENSES/` |
| OkHttp | 4.12.0 | [Apache-2.0](https://github.com/square/okhttp/blob/parent-4.12.0/LICENSE.txt); separately resolved by Gradle |
| JSON-java | 20240303, JVM tests only | [Public domain dedication](https://github.com/stleary/JSON-java/blob/20240303/LICENSE) |
| Lucide / Feather-derived header icons | Database, battery, circle | [ISC / MIT notices](android_app/app/src/main/assets/lucide-LICENSE.txt) |
| MindyLink-derived optional integration | MindyLink v2 compatibility, owner-authorized reuse dated 2026-09-22 | Copyright MindyLab MB; [separate upstream licence](LICENSES/MindyLink.md) and scope below; **not relicensed under Apache-2.0** |

Nordic's 5-clause components are limited to Nordic integrated circuits and
include restrictions on reverse engineering supplied binaries. Do not describe
the complete linked firmware as unrestricted portable Apache-only software.

## MindyLink integration exception

The optional Android MindyLink compatibility layer reuses the owner's MindyLink
protocol/source work with permission recorded during development. In particular,
`android_app/app/src/main/java/org/openpendant/app/MindyLinkProtocol.kt` retains
the MindyLab copyright/provenance notice. The related `MindyLink*.kt` client,
storage, controller and UI files implement that integration. Upstream-derived
portions remain subject to the separate licence, not this repository's general
Apache-2.0 grant. This publication does not grant new rights to reuse or relicense
MindyLink itself; obtain the upstream permission required by its terms.

The separate MindyLink gateway and Forge desktop source/binaries are not bundled
here. Their availability, accounts, commercial terms and model-provider licences
are separate from the core RePendant recording and Bluetooth workflows.

Model weights are not included. Any separately imported model keeps its own
license. The factory research image is a recovered vendor application, **not
our original code and not granted an Apache license by this repository**. It is
documented separately, with no complete-restoration claim.
