package org.openpendant.app

/** Main-thread-only best-effort link hint policy. This grants no radio authority:
 * the client must validate its current connection and exact lease first. */
internal class DurableTransferPriority {
    private var requested: DurableTransportLease? = null
    fun begin(lease: DurableTransportLease): Boolean {
        check(lease.isActive())
        if (requested === lease) return false
        check(requested == null)
        requested = lease
        return true
    }
    /** Clear locally even if Android refuses the balancing hint or close fails.
     * Actual ownership remains fenced by the separate radio-close proof. */
    fun clear(): Boolean = (requested != null).also { requested = null }
}
