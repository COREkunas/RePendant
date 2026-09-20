package org.openpendant.app

/** Read-only gauge snapshot. SOC is a gauge estimate, never an endurance claim. */
data class BatteryTelemetry(val flags: Int, val percent: Int?, val millivolts: Int?,
    val temperatureDecikelvin: Int?, val ageMs: Long, val sequence: Long, val gaugeFlags: Int) {
    val valid get()=flags and 1 != 0
    val startPowerReady get()=flags and 2 != 0
    val portable get()=flags and 4 != 0
    val stopped get()=flags and 8 != 0
    companion object {
        fun parse(p: ByteArray, available: Boolean): BatteryTelemetry {
            ensure(p.size==72 && p[0].toInt()==3, "Invalid battery schema")
            val flags=p[27].toInt() and 255
            ensure(flags and 15==flags && flags and 4!=0 && (flags and 1!=0)==available &&
                (flags and 2==0 || available) && (flags and 8==0 || flags and 3==0), "Invalid battery flags")
            ensure(p[42].toInt()==0 && p[43].toInt()==0, "Reserved battery fields")
            if(!available){
                ensure(p[26].toInt()==-1 && (28..41).all { p[it].toInt()==0 }, "Noncanonical unavailable battery")
                return BatteryTelemetry(flags,null,null,null,0,0,0)
            }
            val soc=p[26].toInt() and 255;val mv=OpProtocol.u16(p,28);val temp=OpProtocol.u16(p,30)
            val age=OpProtocol.u32(p,32);val sequence=OpProtocol.u32(p,36);val gauge=OpProtocol.u16(p,40)
            ensure(soc<=100 && mv in 1..6000 && temp in 2300..3700 && age<=20000 && sequence>0 &&
                gauge and 8!=0 && gauge and 0xfc30==0, "Untrusted battery sample")
            ensure(flags and 2==0 || mv in 3800..4450 && soc>=25 && temp in 2781..3131, "Invalid start-power claim")
            return BatteryTelemetry(flags,soc,mv,temp,age,sequence,gauge)
        }
    }
}
