package org.openpendant.app

/** Foreground connection-only recovery. No command replay or persistent startup
 * intent. Manual disconnect, different bond and failures revoke the intent.
 * Three restarts per two-minute window prevent a flapping radio from looping. */
internal class ConnectionRecovery {
    private var wanted: String? = null
    private var pending = false
    private var due = 0L
    private var window = 0L
    private var attempts = 0
    fun explicitConnect(bond: String, now: Long) {
        wanted=bond;pending=false;window=now;attempts=0
    }
    fun stop() { wanted=null;pending=false }
    fun closed(bond: String?, now: Long, planned: Boolean = false) {
        if(bond==null||bond!=wanted)return
        if(planned){window=now;attempts=0}
        pending=true;due=now+750
    }
    fun claim(bond: String?, now: Long, foreground: Boolean, available: Boolean): Boolean {
        if(!pending||!foreground||!available)return false
        if(bond!=wanted||now<window){stop();return false}
        if(now<due)return false
        if(now-window>=120_000){window=now;attempts=0}
        pending=false
        if(attempts>=3){stop();return false}
        attempts++;return true
    }
    val waiting get()=pending
}
