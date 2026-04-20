// index.c — Staging area (text-based index)

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define INDEX_FILE ".pes/index"

// ─── LOAD INDEX ────────────────────────────────────────────

int index_load(Index *idx) {

    FILE *f = fopen(INDEX_FILE, "r");

    // If file doesn't exist → empty index
    if (!f) {
        idx->count = 0;
        return 0;
    }

    idx->count = 0;

    char line[1024];

    while (fgets(line, sizeof(line), f)) {

        IndexEntry *e = &idx->entries[idx->count];

        char hash_hex[HASH_HEX_SIZE + 1];

        // Parse line
        sscanf(line, "%o %64s %ld %ld %s",
               &e->mode,
               hash_hex,
               &e->mtime,
               &e->size,
               e->path);

        // Convert hex → binary hash
        hex_to_hash(hash_hex, &e->hash);

        idx->count++;
    }

    fclose(f);
    return 0;
}

// ─── SORT HELPER ───────────────────────────────────────────

static int compare_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path,
                  ((IndexEntry *)b)->path);
}

// ─── SAVE INDEX (ATOMIC) ───────────────────────────────────

int index_save(Index *idx) {

    // Sort entries
    qsort(idx->entries, idx->count, sizeof(IndexEntry), compare_entries);

    char tmp_path[] = ".pes/index.tmp";

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    // Write entries
    for (int i = 0; i < idx->count; i++) {

        IndexEntry *e = &idx->entries[i];

        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hash_hex);

        fprintf(f, "%o %s %ld %ld %s\n",
                e->mode,
                hash_hex,
                e->mtime,
                e->size,
                e->path);
    }

    fflush(f);

    // fsync
    int fd = fileno(f);
    fsync(fd);

    fclose(f);

    // atomic rename
    if (rename(tmp_path, INDEX_FILE) != 0)
        return -1;

    return 0;
}

// ─── ADD FILE ──────────────────────────────────────────────

int index_add(const char *path) {

    Index idx;
    index_load(&idx);

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    // Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *buf = malloc(st.st_size);
    fread(buf, 1, st.st_size, f);
    fclose(f);

    // Write blob object
    ObjectID id;
    if (object_write(OBJ_BLOB, buf, st.st_size, &id) != 0) {
        free(buf);
        return -1;
    }

    free(buf);

    // Check if already exists
    int pos = index_find(&idx, path);

    IndexEntry *e;

    if (pos >= 0) {
        e = &idx.entries[pos];   // update existing
    } else {
        e = &idx.entries[idx.count++];  // new entry
    }

    strcpy(e->path, path);
    e->mode = st.st_mode;
    e->mtime = st.st_mtime;
    e->size = st.st_size;
    e->hash = id;

    return index_save(&idx);
}
