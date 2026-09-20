#include <jni.h>
#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr jsize kMaximumSamples = 16000 * 30;
std::mutex inference_mutex;
std::once_flag log_setup;

void no_log(ggml_log_level, const char *, void *) {}

void throw_java(JNIEnv *env, const char *type, const char *message) {
    if (env->ExceptionCheck()) return;
    jclass cls = env->FindClass(type);
    if (cls != nullptr) {
        env->ThrowNew(cls, message);
        env->DeleteLocalRef(cls);
    }
}

// Attach only when a callback arrives on a ggml-owned worker thread. Never share
// a JNIEnv between threads, and detach only threads attached by this scope.
struct CallbackEnv {
    JavaVM *vm;
    JNIEnv *env = nullptr;
    bool attached = false;
    explicit CallbackEnv(JavaVM *value) : vm(value) {
        const jint status = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
            if (!attached) env = nullptr;
        } else if (status != JNI_OK) {
            env = nullptr;
        }
    }
    ~CallbackEnv() { if (attached) vm->DetachCurrentThread(); }
};

struct Callbacks {
    JavaVM *vm = nullptr;
    jobject object = nullptr;
    jmethodID cancelled_method = nullptr;
    jmethodID progress_method = nullptr;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> failed{false};
    std::atomic<int> last_progress{-1};

    bool is_cancelled() {
        if (cancelled.load() || failed.load()) return true;
        CallbackEnv scope(vm);
        if (scope.env == nullptr) {
            failed.store(true);
            return true;
        }
        const jboolean result = scope.env->CallBooleanMethod(object, cancelled_method);
        if (scope.env->ExceptionCheck()) {
            // Exceptions cannot cross arbitrary compute threads. Convert to one
            // explicit failure on the original caller after all workers finish.
            scope.env->ExceptionClear();
            failed.store(true);
            return true;
        }
        if (result == JNI_TRUE) cancelled.store(true);
        return cancelled.load();
    }

    void progress(int percent) {
        if (is_cancelled()) return;
        percent = std::clamp(percent, 0, 100);
        int previous = last_progress.load();
        while (percent > previous) {
            if (last_progress.compare_exchange_weak(previous, percent)) {
                CallbackEnv scope(vm);
                if (scope.env == nullptr) {
                    failed.store(true);
                    return;
                }
                scope.env->CallVoidMethod(object, progress_method, static_cast<jint>(percent));
                if (scope.env->ExceptionCheck()) {
                    scope.env->ExceptionClear();
                    failed.store(true);
                }
                return;
            }
        }
    }
};

struct GlobalCallbackRef {
    JNIEnv *env;
    jobject object;
    ~GlobalCallbackRef() { if (object != nullptr) env->DeleteGlobalRef(object); }
};

// Called only on the original JNI thread, never from an upstream callback.
bool cancelled_or_fail(Callbacks &callbacks) {
    const bool cancelled = callbacks.is_cancelled();
    if (callbacks.failed.load()) throw std::runtime_error("The transcription callback failed.");
    return cancelled;
}

struct LocalPcm {
    std::vector<float> samples;
    explicit LocalPcm(size_t size) : samples(size) {}
    ~LocalPcm() {
        // This scratch copy is not retained in a cache after inference.
        volatile float *data = samples.data();
        for (size_t i = 0; i < samples.size(); ++i) data[i] = 0;
    }
};

std::string string_value(JNIEnv *env, jstring value) {
    if (value == nullptr) throw std::invalid_argument("Missing transcription input.");
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) throw std::runtime_error("Unable to read transcription input.");
    struct ReleaseChars {
        JNIEnv *env;
        jstring value;
        const char *chars;
        ~ReleaseChars() { env->ReleaseStringUTFChars(value, chars); }
    } release{env, value, chars};
    return std::string(chars);
}

std::string json_string(const char *value) {
    std::string result = "\"";
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            result += '\\';
            result += static_cast<char>(*p);
        } else if (*p < 0x20) {
            result += "\\u00";
            result += hex[*p >> 4];
            result += hex[*p & 15];
        } else {
            result += static_cast<char>(*p);
        }
    }
    return result + '"';
}
} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_openpendant_app_WhisperEngine_nativeTranscribe(
    JNIEnv *env, jobject, jstring model_path, jfloatArray pcm, jstring language, jobject callback) {
    try {
        if (pcm == nullptr || callback == nullptr) throw std::invalid_argument("Missing transcription input.");
        const jsize sample_count = env->GetArrayLength(pcm);
        if (sample_count < 1 || sample_count > kMaximumSamples) {
            throw std::invalid_argument("Audio chunk must contain 1 to 480000 samples.");
        }
        const std::string path = string_value(env, model_path);
        const std::string lang = string_value(env, language);
        if (lang != "lt") throw std::invalid_argument("Expected the Lithuanian language code lt.");

        Callbacks callbacks;
        if (env->GetJavaVM(&callbacks.vm) != JNI_OK) throw std::runtime_error("Java runtime unavailable.");
        callbacks.object = env->NewGlobalRef(callback);
        GlobalCallbackRef callback_ref{env, callbacks.object};
        if (callbacks.object == nullptr) return nullptr;
        jclass cls = env->GetObjectClass(callback);
        if (cls == nullptr) return nullptr;
        callbacks.cancelled_method = env->GetMethodID(cls, "isCancelled", "()Z");
        callbacks.progress_method = env->GetMethodID(cls, "onProgress", "(I)V");
        env->DeleteLocalRef(cls);
        if (env->ExceptionCheck()) return nullptr;

        // Kotlin serializes jobs; this is a second guard on the JNI boundary.
        std::unique_lock<std::mutex> inference_guard(inference_mutex);
        std::call_once(log_setup, [] { whisper_log_set(no_log, nullptr); ggml_log_set(no_log, nullptr); });
        if (cancelled_or_fail(callbacks)) {
            throw_java(env, "java/util/concurrent/CancellationException", "Transcription cancelled.");
            return nullptr;
        }
        callbacks.progress(0);
        if (callbacks.failed.load()) throw std::runtime_error("The transcription callback failed.");
        if (callbacks.is_cancelled()) {
            throw_java(env, "java/util/concurrent/CancellationException", "Transcription cancelled.");
            return nullptr;
        }
        if (cancelled_or_fail(callbacks)) {
            throw_java(env, "java/util/concurrent/CancellationException", "Transcription cancelled.");
            return nullptr;
        }
        LocalPcm local_pcm(static_cast<size_t>(sample_count));
        env->GetFloatArrayRegion(pcm, 0, sample_count, local_pcm.samples.data());
        if (env->ExceptionCheck()) return nullptr;
        for (float sample : local_pcm.samples) {
            if (!std::isfinite(sample) || sample < -1.0f || sample > 1.0f) {
                throw std::invalid_argument("PCM samples must be finite and normalized.");
            }
        }

        whisper_context_params context_params = whisper_context_default_params();
        context_params.use_gpu = false;
        context_params.flash_attn = false;
        std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
            whisper_init_from_file_with_params(path.c_str(), context_params), whisper_free);
        if (!context) throw std::runtime_error("The local speech model could not be loaded.");
        if (!whisper_is_multilingual(context.get()) || whisper_model_n_audio_layer(context.get()) != 6 ||
            whisper_model_n_text_layer(context.get()) != 6) {
            throw std::invalid_argument("This build requires the pinned multilingual base model.");
        }

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        params.language = lang.c_str();
        params.detect_language = false;
        params.translate = false;
        params.no_context = true;
        params.no_timestamps = false;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.print_special = false;
        params.suppress_nst = true;
        params.temperature = 0.0f;
        params.temperature_inc = 0.0f;
        params.greedy.best_of = 1;
        params.abort_callback = [](void *data) { return static_cast<Callbacks *>(data)->is_cancelled(); };
        params.abort_callback_user_data = &callbacks;
        params.encoder_begin_callback = [](whisper_context *, whisper_state *, void *data) {
            return !static_cast<Callbacks *>(data)->is_cancelled();
        };
        params.encoder_begin_callback_user_data = &callbacks;
        params.progress_callback = [](whisper_context *, whisper_state *, int progress, void *data) {
            // Completion is only reported after the final result is available.
            static_cast<Callbacks *>(data)->progress(std::min(progress, 99));
        };
        params.progress_callback_user_data = &callbacks;
        const int result = whisper_full(context.get(), params, local_pcm.samples.data(), sample_count);
        if (cancelled_or_fail(callbacks)) {
            throw_java(env, "java/util/concurrent/CancellationException", "Transcription cancelled.");
            return nullptr;
        }
        if (result != 0) throw std::runtime_error("Local speech recognition failed.");

        const int64_t duration_ms = static_cast<int64_t>(sample_count) * 1000 / 16000;
        std::string json = "{\"segments\":[";
        const int segments = whisper_full_n_segments(context.get());
        for (int i = 0; i < segments; ++i) {
            if (i != 0) json += ',';
            const int64_t start = std::clamp(whisper_full_get_segment_t0(context.get(), i) * 10,
                                           int64_t{0}, duration_ms);
            const int64_t end = std::clamp(whisper_full_get_segment_t1(context.get(), i) * 10,
                                         start, duration_ms);
            json += "{\"startMs\":" + std::to_string(start) + ",\"endMs\":" + std::to_string(end) +
                    ",\"text\":" + json_string(whisper_full_get_segment_text(context.get(), i)) + '}';
        }
        json += "]}";
        callbacks.progress(100);
        if (cancelled_or_fail(callbacks)) {
            throw_java(env, "java/util/concurrent/CancellationException", "Transcription cancelled.");
            return nullptr;
        }
        // Standard UTF-8 bytes, not JNI modified UTF-8, preserve all languages.
        jbyteArray output = env->NewByteArray(static_cast<jsize>(json.size()));
        if (output != nullptr) {
            env->SetByteArrayRegion(output, 0, static_cast<jsize>(json.size()),
                                   reinterpret_cast<const jbyte *>(json.data()));
        }
        return output;
    } catch (const std::bad_alloc &) {
        throw_java(env, "java/lang/OutOfMemoryError", "Insufficient memory for local transcription.");
    } catch (const std::invalid_argument &error) {
        throw_java(env, "java/lang/IllegalArgumentException", error.what());
    } catch (const std::exception &error) {
        throw_java(env, "java/lang/IllegalStateException", error.what());
    } catch (...) {
        throw_java(env, "java/lang/IllegalStateException", "Local transcription failed.");
    }
    return nullptr;
}
