#include "segment_publication.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* POSIX file-kind bits are fixed on Android/Linux; no host struct stat ABI. */
#define OPND_TYPE_MASK 0170000u
#define OPND_DIRECTORY 0040000u
#define OPND_REGULAR 0100000u
#define OPND_PATH_MAX 4096u
#define OPND_CONTAINER_MIN 210u
#define OPND_CONTAINER_MAX 65745u

static size_t bounded_length(const char *value, size_t maximum) {
    size_t n = 0;
    if (!value) return maximum;
    while (n < maximum && value[n]) ++n;
    return n;
}
static int hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static int namespace_valid(const char *value) {
    if (!strcmp(value, "encrypted-segments-v1")) return 1;
    if (strncmp(value, "segment-test-", 13) || strlen(value) != 49) return 0;
    int nonzero = 0, nonff = 0;
    for (size_t i = 0; i < 36; ++i) {
        const char c = value[13 + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else { if (!hex_digit(c)) return 0; nonzero |= c != '0'; nonff |= c != 'f'; }
    }
    return nonzero && nonff;
}
static int directory_valid(const char *value) {
    const size_t n = bounded_length(value, OPND_PATH_MAX);
    if (!n || n == OPND_PATH_MAX || value[0] != '/') return 0;
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)value[i] < 33 || (unsigned char)value[i] > 126 || value[i] == '\\') return 0;
        if (value[i] == '/' && (value[i + 1] == '/' || !value[i + 1] ||
            (value[i + 1] == '.' && (value[i + 2] == '/' || !value[i + 2] ||
             (value[i + 2] == '.' && (value[i + 3] == '/' || !value[i + 3])))))) return 0;
    }
    const char *name = strrchr(value, '/');
    static const char parent[] = "/org.openpendant.app/no_backup";
    const size_t parent_bytes = sizeof(parent) - 1;
    return name && (size_t)(name - value) >= parent_bytes &&
        !memcmp(name - parent_bytes, parent, parent_bytes) && namespace_valid(name + 1);
}
static int same_file(const struct opnd_file_identity *a, const struct opnd_file_identity *b) {
    return a->device == b->device && a->inode == b->inode && a->bytes == b->bytes &&
        a->uid == b->uid && a->mode == b->mode && a->links == b->links;
}

int opnd_segment_publish(const char *directory, const char *slot,
                         const struct opnd_publication_ops *ops) {
    if (!ops || !ops->open_directory || !ops->directory_identity || !ops->child_identity ||
        !ops->rename_no_replace || !ops->close_directory || !directory_valid(directory) ||
        bounded_length(slot, 65) != 64) return -EINVAL;
    for (size_t i = 0; i < 64; ++i) if (!hex_digit(slot[i])) return -EINVAL;
    if (ops->api_level < 30 || !ops->rename_supported) return -ENOSYS;
    char source[70], destination[73];
    memcpy(source, slot, 64); memcpy(source + 64, ".part", 6);
    memcpy(destination, slot, 64); memcpy(destination + 64, ".segment", 9);
    int fd = ops->open_directory(ops->user, directory);
    if (fd < 0) return fd;
    struct opnd_file_identity dir = {0}, before = {0}, after = {0};
    int rc = ops->directory_identity(ops->user, fd, &dir);
    if (!rc && ((dir.mode & OPND_TYPE_MASK) != OPND_DIRECTORY || (dir.mode & 0077u) || dir.uid != ops->uid)) rc = -EACCES;
    if (!rc) rc = ops->child_identity(ops->user, fd, source, &before);
    if (!rc && ((before.mode & OPND_TYPE_MASK) != OPND_REGULAR || before.uid != ops->uid ||
        before.links != 1 || before.bytes < OPND_CONTAINER_MIN || before.bytes > OPND_CONTAINER_MAX)) rc = -EINVAL;
    if (!rc) rc = ops->rename_no_replace(ops->user, fd, source, destination);
    if (!rc) rc = ops->child_identity(ops->user, fd, destination, &after);
    if (!rc && !same_file(&before, &after)) rc = -EIO;
    if (!rc) {
        /* Moved source must be absent. Any other lookup error is not absence. */
        int missing = ops->child_identity(ops->user, fd, source, &after);
        if (missing != -ENOENT) rc = missing < 0 ? missing : -EIO;
    }
    int closed = ops->close_directory(ops->user, fd);
    if (!rc && closed) rc = closed;
    /* Caller must fsync the validated directory before any metadata receipt.
     * Any post-rename error retains the file for explicit re-verification. */
    return rc;
}
