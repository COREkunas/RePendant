package org.openpendant.app

import android.content.Context
import android.os.Looper
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.StandardOpenOption.*
import java.security.MessageDigest

/** No secret material in the intent journal; old phone files/keys are retained. */
internal object AndroidKeyReset {
    private val monitor = Any()
    private fun root(context: Context) = context.noBackupFilesDir.canonicalFile.toPath().resolve("key-reset-plans-v1")
    fun plans(context: Context): List<KeyResetProtocol.Plan> = synchronized(monitor) {
        check(Looper.myLooper()!=Looper.getMainLooper())
        val root=root(context)
        val attr=AndroidDurableBinding.attributes(root) ?: return@synchronized emptyList()
        check(attr.isDirectory && root.toRealPath()==root)
        Files.newDirectoryStream(root).use { stream ->
            val plans=mutableListOf<KeyResetProtocol.Plan>()
            for(path in stream) {
                check(plans.size<64 && AndroidDurableBinding.attributes(path)?.isRegularFile==true)
                FileChannel.open(path,READ,NOFOLLOW_LINKS).use { file ->
                    check(file.size()==400L);val bytes=ByteArray(400);val b=ByteBuffer.wrap(bytes)
                    while(b.hasRemaining())check(file.read(b)>0)
                    val plan=KeyResetProtocol.decode(bytes);check(path.fileName.toString()=="${plan.next.volume.volumeId}.bin");plans.add(plan)
                }
            }
            plans
        }
    }
    fun save(context: Context,plan: KeyResetProtocol.Plan) = synchronized(monitor) {
        val existing=plans(context)
        if(plan in existing)return@synchronized
        check(existing.none { it.old==plan.old || it.next.volume==plan.next.volume })
        val root=root(context)
        if(AndroidDurableBinding.attributes(root)==null){Files.createDirectory(root);AndroidDurableSegments.syncDirectory(root.parent)}
        check(root.toRealPath()==root)
        val bytes=KeyResetProtocol.encode(plan)
        FileChannel.open(root.resolve("${plan.next.volume.volumeId}.bin"),CREATE_NEW,WRITE,NOFOLLOW_LINKS).use { file ->
            val b=ByteBuffer.wrap(bytes);while(b.hasRemaining())check(file.write(b)>0);file.force(true)
        }
        AndroidDurableSegments.syncDirectory(root);check(plan in plans(context))
    }
    /** Reuse the new phone's existing key when different; otherwise create a
     * separate candidate only through Recovery's explicit Create button. */
    fun candidate(context: Context,oldFingerprint: String): RecipientKeyVault {
        AndroidRecipientProfiles.checkFingerprint(oldFingerprint)
        val legacy=AndroidRecipientProfiles.legacy(context);val summary=legacy.summary()
        if(summary.state==RecipientVaultState.READY && summary.fingerprintHex!=oldFingerprint)return legacy
        val selector=MessageDigest.getInstance("SHA-256").digest(("OpenPendant key-reset candidate v1:"+oldFingerprint).toByteArray(Charsets.US_ASCII))
            .joinToString(""){"%02x".format(it.toInt() and 255)}
        return AndroidRecipientProfiles.profile(context,selector)
    }
    fun publicRecipient(context: Context,oldFingerprint: String): Pair<String,String> {
        val selected=candidate(context,oldFingerprint)
        val summary=selected.summary()
        check(summary.state==RecipientVaultState.READY && summary.backupVerified && summary.fingerprintHex!=oldFingerprint)
        return selected.withActiveKey { key ->
            val fp=key.publicFingerprint().joinToString(""){"%02x".format(it.toInt() and 255)}
            val point=key.publicKeyBytes().joinToString(""){"%02x".format(it.toInt() and 255)}
            // Protect a fingerprint-addressable copy before publishing any
            // destructive public intent; the original vault stays untouched.
            RecipientRecoveryCodec.encode(key).use { backup ->
                val bytes=backup.copyForExplicitExport()
                try { AndroidRecipientProfiles.profile(context,fp).restore(bytes) } finally { bytes.fill(0) }
            }
            fp to point
        }
    }
}
