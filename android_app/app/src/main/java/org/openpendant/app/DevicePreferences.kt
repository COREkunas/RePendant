package org.openpendant.app

/** Exact firmware schema. No phone-only switch is presented as applied. */
data class DevicePreferences(val profile:Int=0,val brightness:Int=32,val recording:Int=1,
    val lowBattery:Int=4,val connected:Int=0,val usb:Int=0,val fault:Int=1,
    val lowPercent:Int=25,val idleDelay:Int=1,val revision:Long=0) {
    fun encode():ByteArray {
        require(profile in 0..2 && brightness in 8..64 && recording in 1..7 && lowBattery in 1..7 &&
            connected in 0..7 && usb in 0..7 && fault in 1..7 && lowPercent in 20..50 &&
            idleDelay in 0..3 && revision in 0..0xffffffffL)
        return byteArrayOf(1,profile.toByte(),brightness.toByte(),recording.toByte(),lowBattery.toByte(),
            connected.toByte(),usb.toByte(),fault.toByte(),lowPercent.toByte(),idleDelay.toByte(),0,0,0,0,0,0)
            .also { OpProtocol.put32(it,12,revision) }
    }
    companion object {
        const val CAPABILITY=1L shl 12
        fun decode(bytes:ByteArray):DevicePreferences {
            require(bytes.size==16 && bytes[0].toInt()==1 && bytes[10].toInt()==0 && bytes[11].toInt()==0)
            fun at(i:Int)=bytes[i].toInt() and 255
            return DevicePreferences(at(1),at(2),at(3),at(4),at(5),at(6),at(7),at(8),at(9),OpProtocol.u32(bytes,12))
                .also { require(it.encode().contentEquals(bytes)) }
        }
        fun accepted(request:DevicePreferences,reply:ByteArray):DevicePreferences {
            require(request.revision<0xffffffffL)
            val value=decode(reply)
            require(value==request.copy(revision=request.revision+1))
            return value
        }
    }
}
