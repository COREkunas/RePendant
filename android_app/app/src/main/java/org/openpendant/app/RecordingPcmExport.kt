package org.openpendant.app

/** Explicit remote transcription export, separate from the strictly local playback
 * sink. The library owns the job, keys and store throughout. No plaintext file or
 * whole-recording allocation. All segments are authenticated before remote begin.
 * Network consumers run OUTSIDE coordinator locks. Gapped captures refuse export. */
internal class RecordingPcmExport(private val core:RecordingSyncContract,
    val manifest:RecordingManifest,private val source:PlaybackCiphertextSource,
    private val keys:PlaybackKeyAccess,private val decoder:OpusPacketDecoder,private val check:()->Unit):AutoCloseable {
    private val ticket=core.beginTranscription(manifest)
    private var measured=false
    private fun current(){check();check(core.isCurrent(ticket))}
    private fun <T> segment(identity:SegmentIdentity,action:(ByteArray)->T):T {
        check();val row=core.snapshot()
        require(row.manifest==manifest&&manifest.finished&&!row.staleVolume&&!row.downloadSuppressed&&
            row.phoneSegments.containsAll(manifest.segments))
        current()
            val plain=RecordingPlayback.authenticated(identity,source,keys,::current)
            try { current();return action(plain).also { current() } } finally { plain.fill(0) }
    }
    fun measure():Long {
        var bytes=0L;var previous:OpusSegment.Layout?=null
        manifest.segments.forEach { identity -> segment(identity) { plain ->
            val layout=OpusSegment.parse(plain,identity.sequence)
            previous?.let { OpusSegment.requireFollowing(it,layout) }
            require(!layout.gapBefore&&(previous==null||previous!!.nextSample==layout.firstSample)) { "This recording has an audio gap; remote export is paused." }
            bytes+=layout.validSamples*2L;require(bytes<=MindyLinkProtocol.MAX_PCM);previous=layout
        } }
        require(bytes>0);measured=true;return bytes
    }
    fun stream(consume:(offset:Long,pcm:ByteArray)->Unit) {
        check(measured);current()
        var offset=0L
        manifest.segments.forEach { identity -> segment(identity) { plain ->
            OpusPcmAssembler.decodeAuthenticated(plain,identity.sequence,decoder).use { decoded ->
                val bytes=decoded.copyPcm16Le()
                try { current();consume(offset,bytes);offset+=bytes.size;current() } finally { bytes.fill(0) }
            }
        } }
    }
    override fun close(){core.cancelWork(ticket)}
}
