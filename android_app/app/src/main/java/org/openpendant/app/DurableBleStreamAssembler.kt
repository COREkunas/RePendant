package org.openpendant.app

/** One explicitly requested, at-most4096B ciphertext range. No timer renewal,
 * reordering, duplicate tolerance, automatic retry or durable acknowledgment.
 * Only a final contiguous exact range can leave this assembler. */
class DurableBleStreamAssembler(payload: ByteArray) {
    private var storage: ByteArray? = null
    private val nonce: ByteArray
    private val start: Long
    private val maximum: Int
    private var total = 0L
    private var received = 0
    init {
        val checked = DurableBleCodec.encodeRequest(DurableBleCodec.FULL_STREAM, 1, payload)
        checked.fill(0)
        nonce = payload.copyOfRange(0, 16)
        start = OpProtocol.u32(payload, 36); maximum = OpProtocol.u16(payload, 40)
        storage = ByteArray(24 + maximum)
    }
    fun accept(body: ByteArray): ByteArray? {
        try {
            val target = checkNotNull(storage)
            require(body.size in 25..(24 + DurableBleCodec.WIDE_MAX_FRAGMENT))
            require(nonce.indices.all { nonce[it] == body[it] })
            val offset = OpProtocol.u32(body, 16); val count = OpProtocol.u32(body, 20)
            require(count in 425L..34357L && (count - 357) % 68 == 0L && start < count)
            require((total == 0L || total == count) && offset == start + received)
            val wanted = minOf(maximum.toLong(), count - start).toInt()
            require(received < wanted && body.size - 24 == minOf(DurableBleCodec.WIDE_MAX_FRAGMENT, wanted - received))
            if (received == 0) body.copyInto(target, 0, 0, 24)
            total = count; body.copyInto(target, 24 + received, 24); received += body.size - 24
            if (received != wanted) return null
            return target.copyOf(24 + wanted).also { close() }
        } catch (failure: Throwable) { close(); throw failure }
    }
    fun close() { storage?.fill(0); storage = null; nonce.fill(0) }
}
