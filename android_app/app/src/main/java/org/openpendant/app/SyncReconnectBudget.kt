package org.openpendant.app

/** Bounded recovery of a known link loss only. Never retries protocol/hash,
 * storage or key failures; never renews the whole job's absolute deadline. */
internal class SyncReconnectBudget(private val began: Long, private val clock: () -> Long) {
    private var attempts = 0
    private var last = began
    private var stopped = false
    fun admit(observedLinkLoss: Boolean, cancelled: Boolean): Boolean {
        val now = clock()
        if (stopped || !observedLinkLoss || cancelled || began < 0 || now < last || now - began >= MAX_JOB_MILLIS || attempts >= MAX_ATTEMPTS) {
            stopped = true; return false
        }
        last = now; attempts++; return true
    }
    val count: Int get() = attempts
    companion object { const val MAX_ATTEMPTS = 3; const val MAX_JOB_MILLIS = 2 * 60 * 60_000L }
}
