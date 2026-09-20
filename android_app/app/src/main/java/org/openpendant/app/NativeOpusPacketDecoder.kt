package org.openpendant.app

import android.os.Looper

/** No eager library load. Fresh decoder state on every bounded call; singleton
 * serialization limits native memory. Native execution is synchronous, so
 * cancellation is checked by the assembler before/after it, not mid-codec.
 */
object NativeOpusPacketDecoder : OpusPacketDecoder {
    private var loaded = false
    @Synchronized override fun decode(packets: ByteArray): ShortArray {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Decode must run off the main thread" }
        require(packets.size in 60..(501 * 60) && packets.size % 60 == 0) { "Invalid packet span" }
        if (!loaded) { System.loadLibrary("openpendant_opus_decode"); loaded = true }
        return decodeNative(packets) ?: throw IllegalStateException("Opus decoding failed")
    }
    private external fun decodeNative(packets: ByteArray): ShortArray?
}
