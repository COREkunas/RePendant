#include "opus_packet_decode.h"
#include <jni.h>
#include <stdlib.h>

JNIEXPORT jshortArray JNICALL Java_org_openpendant_app_NativeOpusPacketDecoder_decodeNative(
    JNIEnv *env,jobject instance,jbyteArray source) {
    (void)instance;
    if(!source) return NULL;
    jsize bytes=(*env)->GetArrayLength(env,source);
    if(bytes<60 || bytes>(jsize)(OPD_MAX_PACKETS*60u) || bytes%60) return NULL;
    size_t samples=((size_t)bytes/60u)*320u;
    uint8_t *packets=malloc((size_t)bytes);
    int16_t *pcm=calloc(samples,sizeof(*pcm));
    jshortArray output=NULL;
    if(!packets || !pcm) goto done;
    (*env)->GetByteArrayRegion(env,source,0,bytes,(jbyte*)packets);
    if((*env)->ExceptionCheck(env)) goto done;
    if(opd_decode_packets(packets,(size_t)bytes,pcm,samples)) goto done;
    output=(*env)->NewShortArray(env,(jsize)samples);
    if(!output) goto done;
    (*env)->SetShortArrayRegion(env,output,0,(jsize)samples,(const jshort*)pcm);
    /* Range is proved above. If the VM nevertheless raises here, preserve its
     * exception and never publish a partial array. Native buffers are wiped;
     * a VM-internal or partially copied Java array is not guaranteed erased. */
    if((*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env,output); output=NULL; }
done:
    if(packets) { opd_wipe(packets,(size_t)bytes); free(packets); }
    if(pcm) { opd_wipe(pcm,samples*sizeof(*pcm)); free(pcm); }
    return output;
}
