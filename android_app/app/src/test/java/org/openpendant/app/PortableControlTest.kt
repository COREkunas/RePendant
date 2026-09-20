package org.openpendant.app
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class PortableControlTest {
    private val boot=UUID.randomUUID();private val op=UUID.randomUUID();private val rec=UUID.randomUUID()
    private fun running()=LongRecordingControlCodec.State(boot,op,rec,LongRecordingControlCodec.Phase.RUNNING,
        0,2048+2+64+256,1,500,0,1000)
    private fun reject(s:LongRecordingControlCodec.State){try{LongRecordingControlCodec.validate(s);fail()}catch(_:IllegalArgumentException){}}
    @Test fun portableRunningKeepsUsbObservationTruthful(){
        val s=running();LongRecordingControlCodec.validate(s);assertFalse(s.has(LongRecordingControlCodec.USB))
        val plugged=s.copy(flags=s.flags or 1);LongRecordingControlCodec.progress(s,plugged)
        LongRecordingControlCodec.progress(plugged,s)
        reject(s.copy(flags=s.flags and 2048.inv()))
    }
    @Test fun cleanLowPowerRequiresPortableAndCompleteDurability(){
        val s=running();val saved=s.copy(phase=LongRecordingControlCodec.Phase.STOPPED,reason=2,
            flags=2048+64+256+60,committed=500)
        LongRecordingControlCodec.progress(s,saved)
        reject(saved.copy(flags=saved.flags and 2048.inv()))
        reject(saved.copy(committed=499));reject(saved.copy(flags=saved.flags and 16.inv()))
    }
    @Test fun capabilityCannotChangeMidOperation(){
        val s=running().copy(flags=running().flags or 1)
        val changed=s.copy(flags=s.flags and 2048.inv())
        LongRecordingControlCodec.validate(changed)
        try{LongRecordingControlCodec.progress(s,changed);fail()}catch(_:IllegalArgumentException){}
        reject(s.copy(flags=s.flags or 4096))
    }
}
