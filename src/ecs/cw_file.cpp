#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "cw_internal.h"
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define TEMP_SUFFIX ".tmp"
#define READ_CHUNK ((size_t)64 * 1024)

const char *cw_file_status_string(cw_file_status status) {
    static const char *const names[] = {
        "ok", "invalid argument", "cannot open file", "read failed", "write failed",
        "file too large", "cannot replace file", "snapshot error"
    };
    return (unsigned)status < sizeof(names) / sizeof(*names) ? names[status] : "unknown status";
}
static void error_clear(cw_file_error *e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->snapshot.entity = CW_INVALID_ENTITY;
    e->snapshot.component = CW_INVALID_COMPONENT;
}
static bool fail(cw_file_error *e, cw_file_status status, int errnum) {
    if (!e) return false;
    error_clear(e);
    e->status = status;
    e->errnum = errnum;
    return false;
}
/* Wraps a memory-level failure whose details are already in e->snapshot. */
static bool fail_snapshot(cw_file_error *e) {
    if (!e) return false;
    e->status = CW_FILE_ERROR_SNAPSHOT;
    e->errnum = 0;
    return false;
}
static char *temp_path(const char *path) {
    size_t n = strlen(path);
    char *temp = (char *)cw_resize(NULL, cw_add(n, sizeof(TEMP_SUFFIX)), 1);
    memcpy(temp, path, n);
    memcpy(temp + n, TEMP_SUFFIX, sizeof(TEMP_SUFFIX));
    return temp;
}
static bool flush_to_disk(FILE *f) {
    if (fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}
static bool replace_file(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}
bool cw_file_write(const char *path, const void *data, size_t size, cw_file_error *error) {
    error_clear(error);
    if (!path || !*path || (!data && size)) return fail(error, CW_FILE_ERROR_ARGUMENT, 0);
    /* The image goes to a sibling temp file that replaces `path` only once it
     * is complete and flushed, so a failure leaves the previous file intact. */
    char *temp = temp_path(path);
    FILE *f = fopen(temp, "wb");
    if (!f) {
        int errnum = errno;
        free(temp);
        return fail(error, CW_FILE_ERROR_OPEN, errnum);
    }
    cw_file_status status = CW_FILE_OK;
    int errnum = 0;
    if ((size && fwrite(data, 1, size, f) != size) || !flush_to_disk(f)) {
        status = CW_FILE_ERROR_WRITE;
        errnum = errno;
    }
    if (fclose(f) != 0 && status == CW_FILE_OK) {
        status = CW_FILE_ERROR_WRITE;
        errnum = errno;
    }
    if (status == CW_FILE_OK && !replace_file(temp, path)) {
        status = CW_FILE_ERROR_REPLACE;
        errnum = errno;
    }
    if (status != CW_FILE_OK) remove(temp);
    free(temp);
    return status == CW_FILE_OK || fail(error, status, errnum);
}
bool cw_file_read(const char *path, size_t max_size, cw_buffer *out, cw_file_error *error) {
    error_clear(error);
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !*path || !out) return fail(error, CW_FILE_ERROR_ARGUMENT, 0);
    if (!max_size) max_size = CW_FILE_DEFAULT_MAX_SIZE;
    FILE *f = fopen(path, "rb");
    if (!f) return fail(error, CW_FILE_ERROR_OPEN, errno);
    cw_writer w;
    cw_writer_init(&w);
    cw_file_status status = CW_FILE_OK;
    int errnum = 0;
    for (;;) {
        /* Read one byte past the limit so an oversized file is detected
         * without reading all of it. */
        size_t room = max_size - w.size;
        if (room < SIZE_MAX) ++room;
        size_t chunk = room < READ_CHUNK ? room : READ_CHUNK;
        unsigned char *slot = (unsigned char *)cw_writer_claim(&w, chunk);
        size_t got = fread(slot, 1, chunk, f);
        w.size -= chunk - got;
        if (w.size > max_size) { status = CW_FILE_ERROR_TOO_LARGE; break; }
        if (got < chunk) {
            if (ferror(f)) { status = CW_FILE_ERROR_READ; errnum = errno; }
            break;
        }
    }
    fclose(f);
    if (status != CW_FILE_OK) {
        cw_writer_destroy(&w);
        return fail(error, status, errnum);
    }
    out->data = cw_writer_release(&w, &out->size);
    return true;
}

bool cw_snapshot_save_file(cw_world *w, const char *path, const cw_snapshot_save_config *config,
                           cw_file_error *error) {
    error_clear(error);
    if (!w) return fail(error, CW_FILE_ERROR_ARGUMENT, 0);
    cw_writer image;
    cw_writer_init(&image);
    /* Encode completely in memory first: an encoding failure never reaches the disk. */
    if (!cw_snapshot_save(w, config, &image, error ? &error->snapshot : NULL)) {
        cw_writer_destroy(&image);
        return fail_snapshot(error);
    }
    bool ok = cw_file_write(path, image.data, image.size, error);
    cw_writer_destroy(&image);
    return ok;
}
bool cw_snapshot_load_file(cw_world *w, const char *path, const cw_snapshot_load_config *config,
                           cw_file_error *error) {
    cw_buffer image;
    if (!w) return fail(error, CW_FILE_ERROR_ARGUMENT, 0);
    if (!cw_file_read(path, 0, &image, error)) return false;
    bool ok = cw_snapshot_load(w, image.data, image.size, config, error ? &error->snapshot : NULL);
    cw_buffer_free(&image);
    return ok || fail_snapshot(error);
}
bool cw_world_serialize_file(cw_world *w, const char *path, const cw_serialize_config *config,
                             cw_file_error *error) {
    error_clear(error);
    cw_buffer image;
    if (!cw_world_serialize(w, config, &image, error ? &error->snapshot : NULL)) return fail_snapshot(error);
    bool ok = cw_file_write(path, image.data, image.size, error);
    cw_buffer_free(&image);
    return ok;
}
bool cw_world_deserialize_file(cw_world *w, const char *path, const cw_deserialize_config *config,
                               cw_file_error *error) {
    cw_buffer image;
    if (!w) return fail(error, CW_FILE_ERROR_ARGUMENT, 0);
    if (!cw_file_read(path, 0, &image, error)) return false;
    bool ok = cw_world_deserialize(w, image.data, image.size, config, error ? &error->snapshot : NULL);
    cw_buffer_free(&image);
    return ok || fail_snapshot(error);
}
