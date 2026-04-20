// tree.c — Tree object serialization and construction
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#define MODE_FILE  0100644
#define MODE_EXEC  0100755
#define MODE_DIR   0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))   return MODE_DIR;
    if (st.st_mode & S_IXUSR)  return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', (size_t)(end - ptr));
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = (size_t)(space - ptr);
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', (size_t)(end - ptr));
        if (!null_byte) return -1;

        size_t name_len = (size_t)(null_byte - ptr);
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted = *tree;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", e->mode, e->name);
        offset += (size_t)written + 1;  // +1 for '\0'
        memcpy(buffer + offset, e->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: IMPLEMENTED ──────────────────────────────────────────────────────

// Helper: write a single tree level.
// entries[]  - all index entries for the current subtree scope
// count      - number of entries in this scope
// prefix     - the directory prefix being processed (e.g. "src/") — empty for root
// id_out     - receives the ObjectID of the written tree object
//
// Strategy:
//   Walk the entries. For each one:
//     - If it has NO '/' after stripping the prefix → it's a direct file; add blob entry.
//     - If it HAS a '/' → it belongs to a subdirectory; collect all entries sharing
//       that subdirectory prefix, recurse to get the subtree hash, add tree entry.
static int write_tree_level(const IndexEntry *entries, int count,
                            const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    size_t prefix_len = strlen(prefix);
    int i = 0;

    while (i < count) {
        const char *rel = entries[i].path + prefix_len;  // path relative to this level
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // Direct file at this level — add as blob entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // File inside a subdirectory — get the subdirectory name
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the full prefix for this subdirectory
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);
            size_t sub_prefix_len = strlen(sub_prefix);

            // Collect all entries that belong to this subdirectory
            int sub_start = i;
            while (i < count && strncmp(entries[i].path, sub_prefix, sub_prefix_len) == 0) {
                i++;
            }
            int sub_count = i - sub_start;

            // Recurse to write the subtree
            ObjectID sub_tree_id;
            if (write_tree_level(entries + sub_start, sub_count, sub_prefix, &sub_tree_id) != 0)
                return -1;

            // Add the subtree entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub_tree_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
        }
    }

    // Serialize and write the tree object
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

// Comparator to sort index entries by path (for consistent tree hashing)
static int compare_index_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int tree_from_index(ObjectID *id_out) {
    // Load the current index
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) {
        // Empty tree: write an empty tree object
        Tree empty;
        empty.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }

    // Sort entries by path for deterministic processing
    qsort(index.entries, (size_t)index.count, sizeof(IndexEntry), compare_index_by_path);

    // Recursively build tree from root (empty prefix)
    return write_tree_level(index.entries, index.count, "", id_out);
}
