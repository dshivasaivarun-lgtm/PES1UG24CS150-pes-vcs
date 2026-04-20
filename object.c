// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

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

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
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

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {

    const char *type_str =
        (type == OBJ_BLOB) ? "blob" :
        (type == OBJ_TREE) ? "tree" :
        (type == OBJ_COMMIT) ? "commit" : NULL;

    if (!type_str) return -1;

    // Build header
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    header[header_len++] = '\0';   // FIXED (important)

    size_t total = header_len + len;
    unsigned char *buf = malloc(total);
    if (!buf) return -1;

    memcpy(buf, header, header_len);
    memcpy(buf + header_len, data, len);

    // Compute hash
    ObjectID id;
    compute_hash(buf, total, &id);

    if (id_out) *id_out = id;

    // Deduplication
    if (object_exists(&id)) {
        free(buf);
        return 0;
    }

    // Build path
    char path[512];
    object_path(&id, path, sizeof(path));

    // Extract directory
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);   // FIXED (safe copy)

    char *slash = strrchr(dir, '/');
    if (!slash) {
        free(buf);
        return -1;
    }
    *slash = '\0';

    mkdir(".pes", 0755);
    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir, 0755);

    // Temp file
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    // Write safely
    ssize_t written = write(fd, buf, total);   // FIXED
    if (written < 0 || (size_t)written != total) {
        close(fd);
        free(buf);
        return -1;
    }

    fsync(fd);
    close(fd);

    // Atomic rename
    if (rename(tmp, path) != 0) {   // FIXED
        free(buf);
        return -1;
    }

    // fsync directory
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    free(buf);
    return 0;
}


// Read an object from the store.
//
// Returns 0 on success, -1 on error.
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    unsigned char *buf = malloc(size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    fread(buf, 1, size, f);
    fclose(f);

    // Verify hash
    ObjectID check;
    compute_hash(buf, size, &check);

    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    // Find header end
    char *nul = memchr(buf, '\0', size);
    if (!nul) {
        free(buf);
        return -1;
    }

    // Parse type and size
    char type_str[16];
    size_t data_len;

    sscanf((char *)buf, "%s %zu", type_str, &data_len);

    if (strcmp(type_str, "blob") == 0)
        *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0)
        *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0)
        *type_out = OBJ_COMMIT;
    else {
        free(buf);
        return -1;
    }

    // Extract data
    *data_out = malloc(data_len);
    if (!*data_out) {
        free(buf);
        return -1;
    }

    memcpy(*data_out, nul + 1, data_len);
    *len_out = data_len;

    free(buf);
    return 0;
}
