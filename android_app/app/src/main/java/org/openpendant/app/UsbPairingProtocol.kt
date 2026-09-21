package org.openpendant.app

/** Deliberately small USB bootstrap. Never accepts commands from a device,
 * persists credentials, or exports the native Bluetooth/recording keys. */
internal object UsbPairingProtocol {
    const val VID = 0x2fe3
    const val PID = 0x0001
    const val PROMPT = "openpendant> "
    enum class Command(val wire: String) {
        IDENTITY("pairing usbinfo"), STATUS("pairing status"),
        OPEN("pairing open confirm"), CLOSE("pairing close"),
        STORAGE_IDENTITY("recorder fullinfo") // Public, read-only; never formatting.
    }
    data class Identity(val address: String, val type: Int)
    class Status(val bonds: Int, val open: Boolean, val remaining: Long,
                 val pending: Boolean, private var code: ByteArray?) : AutoCloseable {
        val codeReady get() = code != null
        fun takeCode(): ByteArray? = code?.copyOf().also { close() }
        override fun close() { code?.fill(0); code = null }
    }
    private fun lines(text: String): List<String> {
        require(text.length <= 4096 && text.none { it == '\u0000' }) { "Invalid USB reply" }
        return text.split('\n').map { it.trimEnd('\r') }
    }
    fun identity(text: String): Identity {
        val candidates = lines(text).filter { it.startsWith("PAIRING_USB_ID") }
        require(candidates.size == 1) { "Update pendant firmware for USB pairing" }
        val match = Regex("PAIRING_USB_ID v=1 address=([0-9A-F]{2}(?::[0-9A-F]{2}){5}) type=([01])")
            .matchEntire(candidates.single()) ?: error("Unsupported USB pairing identity")
        val address = match.groupValues[1]
        val type = match.groupValues[2].toInt()
        require(address != "00:00:00:00:00:00" && address != "FF:FF:FF:FF:FF:FF")
        require(type == 0 || address.substring(0, 2).toInt(16) and 0xc0 == 0xc0)
        return Identity(address, type)
    }
    fun status(text: String): Status {
        val lines = lines(text)
        val states = lines.filter { it.startsWith("PAIRING_STATUS") }
        require(states.size == 1) { "Invalid USB pairing status" }
        val state = Regex("PAIRING_STATUS rc=0 bonds=([01]) open=([01]) remaining_ms=([0-9]{1,5}) pending=([01]) code_ready=([01]); native bond durability requires reboot/reconnect validation")
            .matchEntire(states.single()) ?: error("Pendant pairing is not ready")
        val bonds = state.groupValues[1].toInt()
        val open = state.groupValues[2] == "1"
        val remaining = state.groupValues[3].toLong()
        val pending = state.groupValues[4] == "1"
        val ready = state.groupValues[5] == "1"
        val codes = lines.filter { it.startsWith("PAIRING_PASSKEY") }
        require(remaining in 0..60000 && open == (remaining > 0))
        require(!ready || (open && pending && bonds == 0))
        require(!pending || open)
        require(bonds == 0 || (!open && !pending && !ready))
        require(codes.size == if (ready) 1 else 0)
        val code = if (ready) {
            val m = Regex("PAIRING_PASSKEY ([0-9]{6}); enter only in your phone's system pairing dialog")
                .matchEntire(codes.single()) ?: error("Invalid pairing code")
            m.groupValues[1].toByteArray(Charsets.US_ASCII)
        } else null
        return Status(bonds, open, remaining, pending, code)
    }
    fun opened(text: String): Boolean = lines(text).count {
        it == "PAIRING_OPEN seconds=60; connect phone, then request pairing status for the passkey; microphone remains off"
    } == 1

    /** Not an Android bond state substitute: both ends must acknowledge success. */
    fun complete(phoneBonded: Boolean, codeSent: Boolean, status: Status) =
        phoneBonded && codeSent && status.bonds == 1 && !status.open && !status.pending && !status.codeReady
}

/** One consented attempt. Android may broadcast more than once; a code must
 * never be submitted twice, for another radio address, or after cancellation. */
internal class UsbPairingAttempt(val address: String, private val deadline: Long) {
    private var request = false
    private var used = false
    private var cancelled = false
    @Synchronized fun pairingRequest(peer: String, variant: Int, bonding: Boolean, now: Long): Boolean {
        // Native Android reports PIN (0) or LE passkey-entry (1). Never confirm
        // a number comparison, passkey-free consent, or display-only variant.
        if (cancelled || used || now >= deadline || peer != address || !bonding || variant !in 0..1) return false
        request = true
        return true
    }
    @Synchronized fun claimCode(peer: String, bonding: Boolean, now: Long): Boolean {
        if (cancelled || used || !request || now >= deadline || peer != address || !bonding) return false
        used = true
        return true
    }
    @Synchronized fun cancel() { cancelled = true }
}
