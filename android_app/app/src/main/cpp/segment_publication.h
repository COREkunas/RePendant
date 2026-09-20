#ifndef OPENPENDANT_SEGMENT_PUBLICATION_H
#define OPENPENDANT_SEGMENT_PUBLICATION_H
#include <stdint.h>

/* Private JNI implementation boundary, not a general filesystem API. All ops
 * return a nonnegative result or negative errno; none may retry/fall back. */
struct opnd_file_identity {
    uint64_t device, inode, bytes;
    uint32_t uid, mode, links;
};
struct opnd_publication_ops {
    void *user;
    int api_level;
    uint32_t uid;
    int rename_supported;
    int (*open_directory)(void *, const char *);
    int (*directory_identity)(void *, int, struct opnd_file_identity *);
    int (*child_identity)(void *, int, const char *, struct opnd_file_identity *);
    int (*rename_no_replace)(void *, int, const char *, const char *);
    int (*close_directory)(void *, int);
};
int opnd_segment_publish(const char *directory, const char *slot,
                         const struct opnd_publication_ops *ops);
#endif
