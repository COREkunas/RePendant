package org.openpendant.app

/** Strict, versioned cached software observations. No inferred gauge/capacity values. */
data class DeviceTelemetry(val uptimeMs: Long, val firmware: String, val microphonePower: Boolean,
    val resourceBusy: Boolean, val red: Int?, val green: Int?, val blue: Int?, val faults: Long,
    val recorder: RecorderTelemetry? = null, val battery: BatteryTelemetry? = null) {
    val ledSummary: String get() = when {
        red == null -> "Unknown"
        red == 0 && green == 0 && blue == 0 -> "Off commanded"
        green == 0 && blue == 0 -> "Red commanded"
        red == 0 && blue == 0 -> "Green commanded"
        red == 0 && green == 0 -> "Blue commanded"
        else -> "RGB $red / $green / $blue"
    }
    companion object {
        const val CAPABILITY = 16L
        fun parse(bytes: ByteArray): DeviceTelemetry {
            ensure((bytes.size == 48 && bytes[0].toInt() == 1 && bytes[1].toInt() == 48) ||
                (bytes.size == 72 && bytes[0].toInt() in 2..3 && bytes[1].toInt() == 72),
                "Unsupported device-status schema")
            val valid = OpProtocol.u16(bytes, 2)
            val batterySchema = bytes[0].toInt() == 3
            ensure(valid == 11 || valid == 15 || batterySchema && valid in setOf(27,31), "Unsupported device-status fields")
            val low = OpProtocol.u32(bytes, 4); val high = OpProtocol.u32(bytes, 8)
            ensure(high <= 0x7fffffffL, "Invalid device uptime")
            ensure(bytes[18].toInt() in 0..1 && bytes[19].toInt() in 0..1, "Invalid device activity")
            ensure((23..25).all { bytes[it].toInt() == 0 }, "Unnegotiated recorder state")
            val battery = if(batterySchema) BatteryTelemetry.parse(bytes, valid and 16 != 0) else {
                ensure(bytes[26].toInt() == -1 && (27..43).all { bytes[it].toInt() == 0 }, "Unnegotiated battery/storage state")
                null
            }
            val faults = OpProtocol.u32(bytes, 44)
            ensure(faults and 3L == faults, "Unknown hardware fault flags")
            val led = valid and 4 != 0
            ensure(led || (20..22).all { bytes[it].toInt() == 0 }, "Unavailable LED fields are not canonical")
            return DeviceTelemetry(low or (high shl 32),
                "${OpProtocol.u16(bytes, 12)}.${OpProtocol.u16(bytes, 14)}.${OpProtocol.u16(bytes, 16)}",
                bytes[18].toInt() == 1, bytes[19].toInt() == 1,
                if (led) bytes[20].toInt() and 255 else null,
                if (led) bytes[21].toInt() and 255 else null,
                if (led) bytes[22].toInt() and 255 else null, faults,
                if(bytes.size==72) RecorderTelemetry.parse(bytes) else null, battery)
        }
    }
}

/** Monotonic receipt time; a disconnection immediately makes a cached value stale. */
data class TimedDeviceTelemetry(val value: DeviceTelemetry, val receivedMs: Long) {
    fun freshBattery(nowMs: Long, connected: Boolean): BatteryTelemetry? {
        val b=value.battery ?: return null
        return b.takeIf { it.valid && fresh(nowMs,connected) && ageMs(nowMs)<=20000L-it.ageMs }
    }
    fun ageMs(nowMs: Long): Long = if (receivedMs >= 0 && nowMs >= receivedMs) nowMs - receivedMs else Long.MAX_VALUE
    fun fresh(nowMs: Long, connected: Boolean): Boolean = connected && ageMs(nowMs) <= 10000
    fun description(nowMs: Long, connected: Boolean): String {
        val age = ageMs(nowMs)
        if (age == Long.MAX_VALUE) return "Stale · clock changed; refresh required"
        if (!connected) return "Stale · disconnected; last connected observation"
        return (if (fresh(nowMs, connected)) "Updated" else "Stale · last updated") + " ${age / 1000}s ago"
    }
}
