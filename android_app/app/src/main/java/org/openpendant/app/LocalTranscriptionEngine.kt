package org.openpendant.app

import org.json.JSONObject

/** Only the pinned JNI engine, with bounded structured output and no logging. */
class LocalTranscriptionEngine : TranscriptionEngine {
    override val version = WhisperEngine.ENGINE_VERSION
    override fun transcribe(modelPath: String, pcm16kMono: FloatArray, language: String,
                            callbacks: TranscriptionEngine.Callbacks): List<TranscriptSegment> {
        val encoded = WhisperEngine.transcribe(modelPath, pcm16kMono, language, object : WhisperEngine.Callbacks {
            override fun isCancelled() = callbacks.isCancelled()
            override fun onProgress(percent: Int) = callbacks.onProgress(percent)
        })
        require(encoded.length <= 256 * 1024) { "Transcription output exceeds the limit" }
        val segments = JSONObject(encoded).getJSONArray("segments")
        require(segments.length() <= 512) { "Too many transcript segments" }
        return List(segments.length()) { index ->
            val segment = segments.getJSONObject(index)
            TranscriptSegment(segment.getLong("startMs"), segment.getLong("endMs"), segment.getString("text"))
        }
    }
}

object SpeechModel {
    // Upstream ggerganov/whisper.cpp revision 5359861c739e955e79d9a303bcbc70fb988958b1.
    val spec = ModelSpec("whisper-base-q5_1", "ggml-base-q5_1.bin", 59_707_625,
        "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898")
}
