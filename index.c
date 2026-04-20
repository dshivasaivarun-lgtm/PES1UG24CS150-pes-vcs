// index.c — Staging area

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INDEX_FILE ".pes/index"

// ─── LOAD INDEX ────────────────────────────────────────────

int index_load(Index *idx) {

    FILE *f = fopen(INDEX_FILE, "rb");
    if (!f) {
        idx->count = 0;
        return 0;
    }

    fread(&idx->count, sizeof(int), 1, f);

    for (int i = 0; i < idx->count; i++) {
        fread(&idx->entries[i], sizeof(IndexEntry), 1, f);
    }

    fclose(f);
    return 0;
}

// ─── SAVE INDEX ────────────────────────────────────────────

int index_save(Index *idx) {

    FILE *f = fopen(INDEX_FILE, "wb");
    if (!f) return -1;

    fwrite(&idx->count, sizeof(int), 1, f);

    for (int i = 0; i < idx->count; i++) {
        fwrite(&idx->entries[i], sizeof(IndexEntry), 1, f);
    }

    fclose(f);
    return 0;
}

// ─── ADD FILE ──────────────────────────────────────────────

int index_add(const char *path) {

    Index idx;
    index_load(&idx);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    void *buf = malloc(size);
    fread(buf, 1, size, f);
    fclose(f);

    ObjectID id;
    object_write(OBJ_BLOB, buf, size, &id);

    free(buf);

    IndexEntry *e = &idx.entries[idx.count++];

    strcpy(e->path, path);
    e->mode = 0100644;
    e->hash = id;

    return index_save(&idx);
}
