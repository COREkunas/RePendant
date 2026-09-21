package org.openpendant.app

import android.hardware.usb.*
import android.os.SystemClock
import java.io.ByteArrayOutputStream
import java.io.IOException

/** Single worker owns all I/O. close() is cancellation-safe from the UI thread.
 * Only a CDC ACM pair on the development pendant VID/PID is accepted; identity
 * comes from the new firmware command, not the USB name or Bluetooth name. */
internal class UsbPairingConsole(manager: UsbManager, private val device: UsbDevice) : AutoCloseable {
    private val connection: UsbDeviceConnection
    private val control: UsbInterface
    private val data: UsbInterface
    private val input: UsbEndpoint
    private val output: UsbEndpoint
    @Volatile private var closed = false
    init {
        require(device.vendorId == UsbPairingProtocol.VID && device.productId == UsbPairingProtocol.PID)
        require(manager.hasPermission(device)) { "USB permission is required" }
        val interfaces = (0 until device.interfaceCount).map(device::getInterface)
        control = interfaces.single { it.interfaceClass == UsbConstants.USB_CLASS_COMM && it.interfaceSubclass == 2 }
        data = interfaces.single { it.interfaceClass == UsbConstants.USB_CLASS_CDC_DATA }
        val endpoints = (0 until data.endpointCount).map(data::getEndpoint)
        require(endpoints.size == 2 && endpoints.all { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK })
        input = endpoints.single { it.direction == UsbConstants.USB_DIR_IN }
        output = endpoints.single { it.direction == UsbConstants.USB_DIR_OUT }
        connection = manager.openDevice(device) ?: throw IOException("Could not open pendant USB")
        try {
            check(connection.claimInterface(control, true) && connection.claimInterface(data, true))
            // 115200, one stop bit, no parity, eight data bits. CDC uses DTR to
            // enable the existing console. No reset/bootloader/vendor requests.
            val coding = byteArrayOf(0x00, 0xc2.toByte(), 0x01, 0x00, 0x00, 0x00, 0x08)
            check(connection.controlTransfer(0x21, 0x20, 0, control.id, coding, coding.size, 1000) == coding.size)
            check(connection.controlTransfer(0x21, 0x22, 1, control.id, null, 0, 1000) == 0)
            // Drain a bounded initial prompt, then establish our own command
            // boundary. No another command or unfinished passkey is reused.
            val scratch = ByteArray(512)
            try {
                val deadline = SystemClock.elapsedRealtime() + 500
                while (SystemClock.elapsedRealtime() < deadline &&
                    connection.bulkTransfer(input, scratch, scratch.size, 50) > 0) { scratch.fill(0) }
            } finally { scratch.fill(0) }
            write("\r".toByteArray(Charsets.US_ASCII))
            readPrompt()
        } catch (e: Exception) { close(); throw e }
    }
    fun request(command: UsbPairingProtocol.Command): String {
        val marker = when(command) {
            UsbPairingProtocol.Command.IDENTITY -> "PAIRING_USB_ID "
            UsbPairingProtocol.Command.STORAGE_IDENTITY -> "RECORDER_FULL_PUBLIC "
            else -> null
        }
        return requestWire(command.wire,marker)
    }
    fun request(command: KeyResetProtocol.Command): String = requestWire(command.wire,when(command){
        KeyResetProtocol.Command.Info -> "RECORDER_KEY_DESCRIPTOR "
        KeyResetProtocol.Command.Restart -> "Restarting application;"
        KeyResetProtocol.Command.Battery -> "BATTERY_WATCH "
        KeyResetProtocol.Command.PauseBattery -> "BATTERY_WATCH_STOP requested=1"
        is KeyResetProtocol.Command.Prepare -> "RECORDER_QUEUED task=18"
        is KeyResetProtocol.Command.Erase -> "RECORDER_QUEUED task=19"
    })
    private fun requestWire(command: String,marker: String?): String {
        check(!closed) { "USB disconnected" }
        require(command.length < 384 && '\r' !in command && '\n' !in command)
        if(command.length>48){
            // The target's RX ring is64B. Drain exact32B echoed prefixes
            // (including80-column wrapping) before sending the next prefix.
            // Never resend a partial command after an uncertain transfer.
            val deadline=SystemClock.elapsedRealtime()+5000
            for(offset in command.indices step 32){
                val piece=command.substring(offset,minOf(offset+32,command.length))
                write(piece.toByteArray(Charsets.US_ASCII))
                val echo=buildString { piece.forEachIndexed { i,c->append(c);if((offset+i+1+UsbPairingProtocol.PROMPT.length)%80==0)append("\r\n") } }
                readExact(echo.toByteArray(Charsets.US_ASCII),deadline)
            }
            write(byteArrayOf(13))
            if((command.length+UsbPairingProtocol.PROMPT.length)%80!=0)readExact(byteArrayOf(13,10),deadline)
        }else{
            val wire = (command + "\r").toByteArray(Charsets.US_ASCII)
            try { write(wire) } finally { wire.fill(0) }
        }
        return readPrompt(marker)
    }
    private fun readExact(expected:ByteArray,deadline:Long){
        var offset=0;val bytes=ByteArray(expected.size)
        try{
            while(offset<expected.size&&!closed&&SystemClock.elapsedRealtime()<deadline){
                val n=connection.bulkTransfer(input,bytes,expected.size-offset,200)
                if(n<=0)continue
                for(i in 0 until n)check(bytes[i]==expected[offset+i]){"Unexpected USB command echo; not retried"}
                offset+=n
            }
            check(offset==expected.size){"USB command echo timed out; not retried"}
        }finally{bytes.fill(0);expected.fill(0)}
    }
    private fun write(bytes: ByteArray) {
        if (closed || connection.bulkTransfer(output, bytes, bytes.size, 1000) != bytes.size)
            throw IOException("USB write did not complete")
    }
    private fun readPrompt(marker: String? = null): String {
        val deadline = SystemClock.elapsedRealtime() + 5000
        val buffer = ByteArray(512)
        val collected = ByteArrayOutputStream()
        try {
            while (!closed && SystemClock.elapsedRealtime() < deadline) {
                val count = connection.bulkTransfer(input, buffer, buffer.size, 200)
                if (count <= 0) continue
                if (collected.size() + count > 4096) throw IOException("USB reply too long")
                collected.write(buffer, 0, count); buffer.fill(0)
                val bytes = collected.toByteArray()
                val text = try { String(bytes, Charsets.US_ASCII) } finally { bytes.fill(0) }
                if (text.endsWith(UsbPairingProtocol.PROMPT) && (marker == null || marker in text)) return text.removeSuffix(UsbPairingProtocol.PROMPT)
            }
            throw IOException("Pendant USB did not reply; check the data cable and firmware")
        } finally {
            buffer.fill(0)
            // Reset does not wipe ByteArrayOutputStream's backing array. Fill
            // it by overwriting the full used capacity before dropping it.
            val size = collected.size(); collected.reset(); collected.write(ByteArray(size)); collected.reset()
        }
    }
    override fun close() {
        if (closed) return
        closed = true
        try { connection.releaseInterface(data) } catch (_: Exception) { }
        try { connection.releaseInterface(control) } catch (_: Exception) { }
        connection.close()
    }
}
