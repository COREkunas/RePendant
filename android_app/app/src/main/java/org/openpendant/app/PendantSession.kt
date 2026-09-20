package org.openpendant.app

import android.content.Context
import android.os.Looper
import android.os.Handler
import android.os.SystemClock

/** Process-owned client/library. Activities observe but never own a running sync.
 * No Activity, key or PCM is retained after detach. No work starts on creation. */
internal class PendantSession private constructor(context: Context) : PendantClient.Listener {
    private val listeners = LinkedHashSet<PendantClient.Listener>()
    val client = PendantClient(context.applicationContext, this)
    val library = AndroidDurableLibrary(context.applicationContext, client, ::changed)
    val longRecording = AndroidLongRecordingController(context.applicationContext, client, ::changed)
    private val handler = Handler(Looper.getMainLooper())
    private var foreground = false
    private var recoveredEpoch: java.util.UUID? = null
    private val maintain = object: Runnable {
        override fun run() {
            if(!foreground)return
            client.recoverForegroundConnection(true,!library.busy&&!longRecording.busy&&!longRecording.ownsRadio)
            handler.postDelayed(this,500)
        }
    }
    fun foreground() {
        main();foreground=true;longRecording.foreground()
        handler.removeCallbacks(maintain);handler.post(maintain)
    }
    fun attach(listener: PendantClient.Listener) { main(); listeners.add(listener) }
    fun detach(listener: PendantClient.Listener) { main(); listeners.remove(listener) }
    override fun changed() {
        main()
        // A recovery is not a new user connection and must not create an
        // automatic sync -> close -> reconnect -> sync loop.
        if(client.recoveredConnection) client.durablePeer()?.let {
            if(it.epoch!=recoveredEpoch){recoveredEpoch=it.epoch;library.transferPolicy.attempted(it,SystemClock.elapsedRealtime())}
        }
        listeners.toList().forEach { it.changed() }
    }
    override fun recordingComplete(pcm: ByteArray) {
        main()
        val visible = listeners.lastOrNull()
        if (visible == null) pcm.fill(0) else visible.recordingComplete(pcm)
    }
    fun background() {
        main()
        foreground=false;handler.removeCallbacks(maintain)
        longRecording.background()
        if (library.state.work != DurableLibraryWork.SYNC) {
            library.cancel(); client.background()
        }
    }
    private fun main() = check(Looper.myLooper() == Looper.getMainLooper())
    companion object {
        private var instance: PendantSession? = null
        fun get(context: Context): PendantSession {
            check(Looper.myLooper() == Looper.getMainLooper())
            return instance ?: PendantSession(context.applicationContext).also { instance = it }
        }
    }
}
