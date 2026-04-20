// index.c — Staging area (text-based index)

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define INDEX_FILE ".pes/index"

// ─── LOAD INDEX ────────────────────────────────────────────

int index_load(Index *index) {

    FILE *f = fopen(INDEX_FILE, "r");

    // If file doesn't exist → empty index
    if (!f) {
        index->count = 0;
        return 0;
    }

    index->count = 0;

    char line[1024];

    while (fgets(line, sizeof(line), f)) {

        IndexEntry *e = &index->entries[index->count];

        char hash_hex[HASH_HEX_SIZE + 1];

        sscanf(line, "%o %64s %lu %u %s",
               &e->mode,
               hash_hex,
               &e->mtime_sec,
               &e->size,
               e->path);

        hex_to_hash(hash_hex, &e->hash);

        index->count++;
    }

    fclose(f);
    return 0;
}

// ─── SORT ─────────────────────────────────────────────────

static int cmp(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path,
                  ((IndexEntry *)b)->path);
}

// ─── SAVE INDEX (ATOMIC WRITE) ─────────────────────────────

int index_save(const Index *index) {

    // Copy to sort
    Index temp = *index;
    qsort(temp.entries, temp.count, sizeof(IndexEntry), cmp);

    char tmp[] = ".pes/index.tmp";

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    for (int i = 0; i < temp.count; i++) {

        const IndexEntry *e = &temp.entries[i];

        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);

        fprintf(f, "%o %s %lu %u %s\n",
                e->mode,
                hex,
                e->mtime_sec,
                e->size,
                e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp, INDEX_FILE) != 0)
        return -1;

    return 0;
}

// ─── ADD FILE ──────────────────────────────────────────────

int index_add(Index *index, const char *path) {

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *buf = malloc(st.st_size);
    fread(buf, 1, st.st_size, f);
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, buf, st.st_size, &id) != 0) {
        free(buf);
        return -1;
    }

    free(buf);

    // Check existing entry
    IndexEntry *e = index_find(index, path);

    if (!e) {
        e = &index->entries[index->count++];
    }

    strcpy(e->path, path);
    e->mode = st.st_mode;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    e->hash = id;

    return index_save(index);
}
