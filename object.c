// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ─────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ───────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str =
        (type == OBJ_BLOB)   ? "blob"   :
        (type == OBJ_TREE)   ? "tree"   :
        (type == OBJ_COMMIT) ? "commit" : NULL;
    if (!type_str) return -1;

    // Build header: "<type> <size>\0"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // Allocate full buffer = header + data
    size_t total = (size_t)header_len + len;
    unsigned char *buf = malloc(total);
    if (!buf) return -1;
    memcpy(buf, header, header_len);
    memcpy(buf + header_len, data, len);

    // Compute SHA-256 of entire buffer
    ObjectID id;
    compute_hash(buf, total, &id);
    if (id_out) *id_out = id;

    // Deduplication: skip if already stored
    if (object_exists(&id)) { free(buf); return 0; }

    // Build shard path: .pes/objects/XX/YYY...
    char path[512];
    object_path(&id, path, sizeof(path));

    // Extract directory portion
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) { free(buf); return -1; }
    *slash = '\0';

    // Create directories
    mkdir(".pes", 0755);
    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir, 0755);

    // Atomic write: write to tmp, fsync, rename
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0444);
    if (fd < 0) { free(buf); return -1; }

    ssize_t written = write(fd, buf, total);
    fsync(fd);
    close(fd);
    free(buf);

    if (written < 0 || (size_t)written != total) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    // fsync shard directory
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    if (file_size <= 0) { fclose(f); return -1; }

    unsigned char *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);

    // Integrity check: re-hash and compare
    ObjectID check;
    compute_hash(buf, (size_t)file_size, &check);
    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) { free(buf); return -1; }

    // Find the '\0' separating header from data
    unsigned char *nul = memchr(buf, '\0', (size_t)file_size);
    if (!nul) { free(buf); return -1; }

    // Parse type and size from header
    char type_str[16];
    size_t data_len = 0;
    if (sscanf((char *)buf, "%15s %zu", type_str, &data_len) != 2) { free(buf); return -1; }

    if      (strcmp(type_str, "blob")   == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree")   == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Copy data portion (after the '\0')
    unsigned char *data_start = nul + 1;
    size_t available = (size_t)file_size - (size_t)(data_start - buf);
    if (data_len > available) { free(buf); return -1; }

    *data_out = malloc(data_len + 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, data_start, data_len);
    ((char *)*data_out)[data_len] = '\0';
    *len_out = data_len;

    free(buf);
    return 0;
}
