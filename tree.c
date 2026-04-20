// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MODE_FILE 0100644
#define MODE_DIR  0040000

// ─── SORTING ───────────────────────────────────────────────

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((TreeEntry *)a)->name, ((TreeEntry *)b)->name);
}

// ─── SERIALIZATION ─────────────────────────────────────────

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max = tree->count * 300;
    uint8_t *buf = malloc(max);
    if (!buf) return -1;

    Tree tmp = *tree;
    qsort(tmp.entries, tmp.count, sizeof(TreeEntry), compare_tree_entries);

    size_t off = 0;

    for (int i = 0; i < tmp.count; i++) {
        TreeEntry *e = &tmp.entries[i];

        int written = sprintf((char *)buf + off, "%o %s", e->mode, e->name);
        off += written + 1;

        memcpy(buf + off, e->hash.hash, HASH_SIZE);
        off += HASH_SIZE;
    }

    *data_out = buf;
    *len_out = off;
    return 0;
}

// ─── TREE BUILDING ─────────────────────────────────────────

static int build_tree(Index *index, const char *prefix, ObjectID *out) {

    Tree tree;
    tree.count = 0;

    size_t plen = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        IndexEntry *e = &index->entries[i];

        if (plen > 0 && strncmp(e->path, prefix, plen) != 0)
            continue;

        const char *rest = e->path + plen;
        if (rest[0] == '/') rest++;

        const char *slash = strchr(rest, '/');

        // FILE
        if (!slash) {
            TreeEntry *te = &tree.entries[tree.count++];

            te->mode = e->mode;
            strcpy(te->name, rest);
            te->hash = e->hash;
        }

        // DIRECTORY
        else {
            char dir[256];
            int len = slash - rest;

            strncpy(dir, rest, len);
            dir[len] = '\0';

            int exists = 0;
            for (int j = 0; j < tree.count; j++) {
                if (strcmp(tree.entries[j].name, dir) == 0) {
                    exists = 1;
                    break;
                }
            }

            if (!exists) {
                TreeEntry *te = &tree.entries[tree.count++];

                te->mode = MODE_DIR;
                strcpy(te->name, dir);

                char new_prefix[512];
                if (plen == 0)
                    snprintf(new_prefix, sizeof(new_prefix), "%s", dir);
                else
                    snprintf(new_prefix, sizeof(new_prefix), "%s/%s", prefix, dir);

                if (build_tree(index, new_prefix, &te->hash) != 0)
                    return -1;
            }
        }
    }

    void *data;
    size_t len;

    if (tree_serialize(&tree, &data, &len) != 0)
        return -1;

    int rc = object_write(OBJ_TREE, data, len, out);

    free(data);
    return rc;
}

// ─── MAIN FUNCTION ─────────────────────────────────────────

int tree_from_index(ObjectID *id_out) {
    Index index;

    if (index_load(&index) != 0)
        return -1;

    return build_tree(&index, "", id_out);
}
