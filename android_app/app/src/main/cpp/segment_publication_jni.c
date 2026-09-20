#define _GNU_SOURCE 1
#include "segment_publication.h"
#include <android/api-level.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int (*renameat2_fn)(int, const char *, int, const char *, unsigned);
struct syscall_context { renameat2_fn rename_function; };
static int negative_errno(void) { return errno > 0 ? -errno : -EIO; }
static void identity(struct opnd_file_identity *output, const struct stat *value) {
    output->device = (uint64_t)value->st_dev; output->inode = (uint64_t)value->st_ino;
    output->bytes = (uint64_t)value->st_size; output->uid = (uint32_t)value->st_uid;
    output->mode = (uint32_t)value->st_mode; output->links = (uint32_t)value->st_nlink;
}
static int open_directory(void *user, const char *path) {
    (void)user;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return fd < 0 ? negative_errno() : fd;
}
static int directory_identity(void *user, int fd, struct opnd_file_identity *output) {
    (void)user; struct stat value;
    if (fstat(fd, &value)) return negative_errno();
    identity(output, &value); return 0;
}
static int child_identity(void *user, int fd, const char *name, struct opnd_file_identity *output) {
    (void)user; struct stat value;
    if (fstatat(fd, name, &value, AT_SYMLINK_NOFOLLOW)) return negative_errno();
    identity(output, &value); return 0;
}
static int rename_no_replace(void *user, int fd, const char *source, const char *destination) {
    struct syscall_context *context = (struct syscall_context *)user;
    /* RENAME_NOREPLACE=1. No link, rename, copying, syscall or overwrite fallback. */
    if (context->rename_function(fd, source, fd, destination, 1u)) return negative_errno();
    return 0;
}
static int close_directory(void *user, int fd) {
    (void)user; return close(fd) ? negative_errno() : 0;
}
JNIEXPORT jint JNICALL Java_org_openpendant_app_AndroidSegmentPublication_publishNative(
    JNIEnv *env, jobject receiver, jstring directory, jstring slot) {
    (void)receiver;
    if (!directory || !slot) return -EINVAL;
    if ((*env)->GetStringUTFLength(env, directory) >= 4096 || (*env)->GetStringLength(env, slot) != 64) return -EINVAL;
    const int api = android_get_device_api_level();
    if (api < 30) return -ENOSYS;
    /* Resolve at runtime, preserving minSdk26 library load compatibility. Older
     * or unsupported providers fail closed rather than calling a raw syscall. */
    struct syscall_context context = { (renameat2_fn)dlsym(RTLD_DEFAULT, "renameat2") };
    if (!context.rename_function) return -ENOSYS;
    const char *root = (*env)->GetStringUTFChars(env, directory, NULL);
    if (!root) return -ENOMEM;
    const char *name = (*env)->GetStringUTFChars(env, slot, NULL);
    if (!name) { (*env)->ReleaseStringUTFChars(env, directory, root); return -ENOMEM; }
    const struct opnd_publication_ops ops = { &context, api, (uint32_t)geteuid(), 1,
        open_directory, directory_identity, child_identity, rename_no_replace, close_directory };
    const int rc = opnd_segment_publish(root, name, &ops);
    (*env)->ReleaseStringUTFChars(env, slot, name);
    (*env)->ReleaseStringUTFChars(env, directory, root);
    return (jint)rc;
}
