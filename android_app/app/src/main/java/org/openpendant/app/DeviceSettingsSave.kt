package org.openpendant.app

import java.util.UUID

/** One explicitly confirmed request, bound to the current link and base revision.
 * Waiting is only for a status READ. A submitted write is never retried. */
internal class DeviceSettingsSave {
    enum class Phase { IDLE, CHECKING, WRITING, VERIFYING, SAVED, NOT_SENT, UNKNOWN }
    @Volatile var phase=Phase.IDLE
        private set
    var message:String?=null
        private set
    var request:DevicePreferences?=null
        private set
    private var base:DevicePreferences?=null
    private var epoch:UUID?=null
    private var deadline=0L
    val busy get()=phase in setOf(Phase.CHECKING,Phase.WRITING,Phase.VERIFYING)
    fun refuse(reason:String):Boolean {
        if(!busy && phase!=Phase.UNKNOWN){phase=Phase.NOT_SENT;message="Not saved. $reason"}
        return false
    }
    fun begin(value:DevicePreferences,current:DevicePreferences?,link:UUID?,now:Long):Boolean {
        if(busy||phase==Phase.UNKNOWN)return false
        if(link==null||current==null)return refuse("Connect and read pendant settings first.")
        if(value.revision!=current.revision)return refuse("Settings changed. Read them again before saving.")
        if(value.revision==0xffffffffL)return refuse("Settings revision is exhausted.")
        try{value.encode()}catch(_:IllegalArgumentException){return refuse("A setting is outside its supported range.")}
        request=value;base=current;epoch=link;deadline=now+6000
        phase=Phase.CHECKING;message="Checking pendant before saving…"
        return true
    }
    /** Called only after a validated fresh status reply, before the sole SET. */
    fun admit(link:UUID?,current:DevicePreferences?,now:Long,powerRefusal:String?):Boolean {
        if(phase!=Phase.CHECKING)return false
        val reason=when {
            link!=epoch -> "Connection changed. Connect and read settings again."
            now>=deadline -> "Status check took too long. Nothing was sent."
            current!=base -> "Settings changed. Read them again before saving."
            else -> powerRefusal
        }
        if(reason!=null){phase=Phase.NOT_SENT;message="Not saved. $reason";return false}
        phase=Phase.WRITING;message="Saving on pendant…"
        return true
    }
    fun acknowledged(reply:ByteArray):DevicePreferences {
        check(phase==Phase.WRITING)
        val value=DevicePreferences.accepted(checkNotNull(request),reply)
        phase=Phase.VERIFYING;message="Checking saved settings…"
        return value
    }
    fun verified(value:DevicePreferences) {
        check(phase==Phase.VERIFYING)
        check(value==checkNotNull(request).let{it.copy(revision=it.revision+1)})
        phase=Phase.SAVED;message="Saved on pendant · verified."
    }
    fun disconnected() {
        when(phase){
            Phase.CHECKING -> {phase=Phase.NOT_SENT;message="Not saved. Connection interrupted before sending. Your changes are kept."}
            Phase.WRITING,Phase.VERIFYING -> {phase=Phase.UNKNOWN;message="Save not confirmed. Reconnect and read settings before trying again. Your changes are kept."}
            else -> Unit
        }
    }
    /** Explicit read reconciles uncertainty; it never sends another write. */
    fun readObserved(value:DevicePreferences) {
        if(phase!=Phase.UNKNOWN)return
        val wanted=checkNotNull(request)
        if(value==wanted.copy(revision=wanted.revision+1)){
            phase=Phase.SAVED;message="Saved on pendant · verified after reconnect."
        }else{
            phase=Phase.NOT_SENT;message="Current pendant settings read. Review your unsaved changes before saving again."
        }
    }
    fun reset(){check(!busy);phase=Phase.IDLE;message=null;request=null;base=null;epoch=null}
}

/** Phone-side edits survive status refresh and disconnect, but never cross a
 * selected device or silently overwrite a different pendant revision. */
internal class DeviceSettingsDraft {
    var loaded:DevicePreferences?=null; private set
    var draft:DevicePreferences?=null; private set
    var generation=0; private set
    private var base:DevicePreferences?=null
    private var peer:String?=null
    val dirty get()=draft!=null&&draft!=base
    val conflict get()=dirty&&loaded!=null&&loaded!=base
    fun observe(address:String?,value:DevicePreferences?) {
        if(address!=peer){peer=address;loaded=null;draft=null;base=null;generation++}
        loaded=value
        if(value!=null&&(!dirty||draft?.let{it.copy(revision=value.revision)==value&&value.revision>=it.revision}==true))adopt(value)
    }
    fun edit(change:DevicePreferences.()->DevicePreferences){draft=draft?.change()}
    fun discard(){loaded?.let(::adopt)}
    private fun adopt(value:DevicePreferences){if(draft!=value)generation++;base=value;draft=value}
}
