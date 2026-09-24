package org.openpendant.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager

/** User-started, non-exported transfer service. Never starts capture,
 * pairing, enrollment, boot work or process-death retries. Stop preserves all
 * checkpoints. No unbounded wakelock or sticky service resurrection. */
class StorageSyncService : Service() {
    private val main = Handler(Looper.getMainLooper())
    private var request: BackgroundTransfer? = null
    private var wake: PowerManager.WakeLock? = null
    private var began = 0L
    private val update = object : Runnable {
        override fun run() {
            val own = request ?: return
            if (active !== own || own.finished) {
                stopSelf(); return
            }
            if (android.os.SystemClock.elapsedRealtime() - began >= MAX_MILLIS) {
                own.stop(); stopSelf(); return
            }
            try {
                getSystemService(NotificationManager::class.java).notify(NOTIFICATION, notification(own))
                main.postDelayed(this, 1000)
            } catch (_: Exception) { own.stop(); stopSelf() }
        }
    }
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val own = active
        if (own == null || intent?.getStringExtra("token") != own.token) {
            if (request == null) stopSelf(startId)
            return START_NOT_STICKY
        }
        if (intent.action == STOP) {
            own.stop()
            // Keep protection until this worker closes its radio/export lease.
            return START_NOT_STICKY
        }
        if (request === own) return START_NOT_STICKY
        request = own
        try {
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "Recording transfers", NotificationManager.IMPORTANCE_LOW))
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(NOTIFICATION, notification(own), if (own.work == DurableLibraryWork.EXPORT)
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            else startForeground(NOTIFICATION, notification(own))
            began = android.os.SystemClock.elapsedRealtime()
            wake = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,
                "OpenPendant:StorageSync").apply { setReferenceCounted(false); acquire(MAX_MILLIS) }
            own.foregroundReady()
            main.removeCallbacks(update); main.post(update)
        } catch (_: Exception) { own.stop(); stopSelf() }
        return START_NOT_STICKY
    }
    private fun notification(own: BackgroundTransfer): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, StorageSyncService::class.java)
            .setAction(STOP).setData(android.net.Uri.parse("openpendant-transfer:${own.token}"))
            .putExtra("token", own.token), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val message = if (own.stopping) "Stopping transfer; saved progress is kept…" else own.message()
        return Notification.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_pendant)
            .setContentTitle(if (own.work == DurableLibraryWork.EXPORT) "Sending recording to Forge" else "Syncing pendant recordings").setContentText(message)
            .setStyle(Notification.BigTextStyle().bigText(message))
            .setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .addAction(Notification.Action.Builder(null, "Stop transfer", stop).build()).build()
    }
    override fun onDestroy() {
        main.removeCallbacks(update)
        request?.let { own ->
            if (active === own) active = null
            own.stop()
        }
        wake?.let { if (it.isHeld) it.release() }; wake = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }
    // Android 15 dataSync budget: stop promptly; never keep running past timeout.
    override fun onTimeout(startId: Int, fgsType: Int) {
        request?.stop()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }
    companion object {
        private const val CHANNEL = "pendant-storage-sync"
        private const val NOTIFICATION = 41
        private const val STOP = "org.openpendant.app.STOP_STORAGE_SYNC"
        const val MAX_MILLIS = 2 * 60 * 60_000L
        private var active: BackgroundTransfer? = null
        internal fun start(context: Context, own: BackgroundTransfer) {
            check(Looper.myLooper() == Looper.getMainLooper())
            check(active == null) { "Previous sync service is still stopping" }
            active = own
            try {
                context.startForegroundService(Intent(context, StorageSyncService::class.java).putExtra("token", own.token))
            } catch (failure: Exception) { if (active === own) active = null; own.stop(); throw failure }
        }
    }
}
