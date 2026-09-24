package org.openpendant.app

import android.content.Context
import android.os.SystemClock

/** Tiny public-metadata preference; asynchronous persistence, no recording/key access. */
internal class AndroidPendantHeaderCache(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences("pendant-header-v1", Context.MODE_PRIVATE)
    var latest = try { prefs.getString("snapshot", null)?.let(PendantHeaderSnapshot::decode) } catch (_: Exception) { null }
        private set
    private var lastPeer: String? = null
    private var lastReceipt = Long.MIN_VALUE
    fun forPeer(peer: String?) = latest?.takeIf { peer == null || it.peer == peer }
    fun observe(peer: String?, timed: TimedDeviceTelemetry?, connected: Boolean) {
        if (peer == lastPeer && timed?.receivedMs == lastReceipt) return
        val next = PendantHeaderSnapshot.observe(latest, peer, timed, connected, SystemClock.elapsedRealtime(), System.currentTimeMillis()) ?: return
        lastPeer = peer; lastReceipt = timed!!.receivedMs; latest = next
        prefs.edit().putString("snapshot", next.encode()).apply()
    }
}
