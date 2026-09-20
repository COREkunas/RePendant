# Battery reconnection recovery — firmware 0.4.59 / Android 0.6.51

This source update follows an observed failure after the battery was removed
and reconnected: the battery monitor stopped during initialization, so both
phone-started and button-started recording were correctly refused by the shared
power check. No leftover diagnostic recording owner was found.

## Firmware behavior

- A reset gauge requiring configuration defers safely until USB is present.
- Automatic coulomb-counter calibration (CCA) is allowed to settle through up
  to 16 successful status observations at the existing four-second query pace.
- Only otherwise-valid CCA status may wait. Other unsafe flags, transport
  failures and partially completed configuration writes are not blindly retried.
- The existing 170-second restoration bound and configuration readback checks
  remain. Bootloader, radio, provisioning and recording-storage format are unchanged.

Implementation anchors: `battery_restore.inc` (`restore_settled_status`),
`battery_service.inc`, `battery_probe.h`, and `recording_battery_monitor.inc`
under `usb_firmware/src`.

The factory application was used as the first behavioral reference. Its gauge
configuration window at 0x45cd4–0x460ca configures the 245 mAh battery, applies
the gain-sign quirk, resets and seals the gauge. No relocatable factory machine
code was copied. TI's [BQ27427 technical reference](https://www.ti.com/lit/ug/sluucd5/sluucd5.pdf),
CONTROL_STATUS table 5-3, distinguishes automatic CCA from other calibration
modes; the change preserves explicit safety admission rather than masking all
calibration conditions.

## Android behavior

- Battery initialization warnings are no longer overwritten by generic button advice.
- USB-only sync fallback no longer claims that battery sync is ready.
- The catalog-reading stage is visible and the final message explicitly describes
  the last completed sync and its recording count.
- Packet formats, recording consent, deletion and automatic-sync policies are unchanged.

## Measured evidence and boundaries

On the development pendant, a full USB/battery disconnect followed by a
battery-only wait and then USB reconnection passed deferred recovery. Initialization
took 114.076 seconds, including 48 restoration transactions / 23 gauge writes;
configuration readback passed, the gauge was sealed and three fresh samples arrived.
Recorder state was healthy/idle, microphone off; the observer caused no NAND I/O.

The subsequent phone regression passed four consecutive empty-catalog syncs and
automatic reconnection after controlled Bluetooth loss, long-status cleanup and
foreground return. Manual disconnect was respected. Five periodic status polls
caused zero unchanged capacity-text rewrites. No recording or deletion was started.

Development tests: 634 Android JVM tests; lint zero errors / 14 existing warnings.
Native protocol model: 420 groups / 1,537,060 assertions, including transient,
persistent and mixed-fault CCA, deferred USB recovery, and cancellation/failure
at status observations. These models do not prove physical electrical behavior.

**Still unresolved:** a fully reset gauge requires USB for restoration before
recording; battery-only cold-start recovery is not implemented. Ordinary USB
unplugging while leaving the battery attached is a different, previously tested
path. The physical run did not encounter CCA during resumed initialization, so
it does not prove a physical CCA-to-clear transition. Empty-catalog sync is not
populated audio-transfer throughput, and no new microphone recording was run
for this qualification. There is no universal Bluetooth-stability claim.

This commit updates source and documentation only. Existing firmware 0.4.57 and
Android 0.6.49 release downloads are unchanged.
