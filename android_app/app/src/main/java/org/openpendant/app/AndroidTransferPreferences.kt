package org.openpendant.app

import android.content.Context

/** Policy only: never stores keys, audio, or a persisted auto-start job. */
internal class AndroidTransferPreferences(context: Context) {
    private val store = context.applicationContext.getSharedPreferences("transfer-preferences-v1", Context.MODE_PRIVATE)
    fun read() = TransferPreferences(store.getBoolean("automatic", false), store.getBoolean("removeAfterSync", false),
        store.getBoolean("lowBattery", false), store.getInt("lowBatteryPercent", 25).takeIf { it in setOf(15, 25, 35) } ?: 25)
    fun save(value: TransferPreferences) {
        require(value.lowBatteryPercent in setOf(15, 25, 35))
        check(store.edit().putBoolean("automatic", value.automatic).putBoolean("removeAfterSync", value.removeAfterSync)
            .putBoolean("lowBattery", value.lowBattery).putInt("lowBatteryPercent", value.lowBatteryPercent).commit())
    }
}
