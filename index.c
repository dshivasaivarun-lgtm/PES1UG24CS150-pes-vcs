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

    if (!f) {
        index->count = 0;
        return 0;
    }

    index->count = 0;

    char line[1024];

    while (fgets(line, sizeof(line), f)) {

        if (index->count >= MAX_INDEX_ENTRIES)
            break;

        IndexEntry *e = &index->entries[index->count];

        char hash_hex[HASH_HEX_SIZE + 1];

        // Safe parse (must read 5 fields)
        if (sscanf(line, "%o %64s %lu %u %511s",
                   &e->mode,
                   hash_hex,
                   &e->mtime_sec,
                   &e->size,
                   e->path) != 5) {
            continue; // skip bad lines
        }

        if (hex_to_hash(hash_hex, &e->hash) != 0)
            continue;

        index->count++;
    }

    fclose(f);
    return 0;
}

// ─── SORT ─────────────────────────────────────────────────

static int cmp(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// ─── SAVE INDEX (ATOMIC) ──────────────────────────────────

int index_save(const Index *index) {

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

// ─── FIND ENTRY ───────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            return &index->entries[i];
        }
    }
    return NULL;
}

// ─── ADD FILE (FIXED SAFE VERSION) ─────────────────────────

int index_add(Index *index, const char *path) {

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *buf = malloc(st.st_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t n = fread(buf, 1, st.st_size, f);
    fclose(f);

    if (n != st.st_size) {
        free(buf);
        return -1;
    }

    ObjectID id;
    if (object_write(OBJ_BLOB, buf, st.st_size, &id) != 0) {
        free(buf);
        return -1;
    }

    free(buf);

    // find existing
    IndexEntry *e = index_find(index, path);

    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES)
            return -1;

        e = &index->entries[index->count++];
    }

    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';

    e->mode = st.st_mode;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    e->hash = id;

    return index_save(index);
}

// ─── REMOVE FILE ──────────────────────────────────────────

int index_remove(Index *index, const char *path) {

    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {

            for (int j = i; j < index->count - 1; j++) {
                index->entries[j] = index->entries[j + 1];
            }

            index->count--;
            return index_save(index);
        }
    }

    return -1;
}

// ─── STATUS (BASIC VERSION) ───────────────────────────────

int index_status(const Index *index) {

    printf("Staged changes:\n");

    if (index->count == 0) {
        printf("  (nothing to show)\n");
    } else {
        for (int i = 0; i < index->count; i++) {
            printf("  staged:     %s\n", index->entries[i].path);
        }
    }

    printf("\nUnstaged changes:\n");
    printf("  (nothing to show)\n");

    printf("\nUntracked files:\n");
    printf("  (nothing to show)\n");

    return 0;
}
