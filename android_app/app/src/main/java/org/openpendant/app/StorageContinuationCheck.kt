package org.openpendant.app

/** Cached observation only, never read/write authority. CCC disable revokes the
 * previous transfer, but its admitted NAND worker must still join and suspend.
 * Authentication, capability and volume checks remain separately mandatory. */
internal object StorageContinuationCheck {
    fun retired(value: DeviceTelemetry, bits:Long=0): Boolean {
        require(!value.microphonePower && value.faults == 0L)
        val recorder = requireNotNull(value.recorder)
        require(!recorder.fault && !recorder.recording)
        require(StorageSyncPower.powerRefusal(TimedDeviceTelemetry(value,0),true,0,bits)==null)
        return recorder.ready && recorder.mounted && !recorder.busy &&
            !value.resourceBusy && recorder.flags and 8 != 0 // RT_SUSPENDED
    }
}
