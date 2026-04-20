// commit.c — Commit creation and history traversal

#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Forward declarations (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ─────────────────────────────────────────────────────

// Parse raw commit data into a Commit struct.
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)len;

    memset(commit_out, 0, sizeof(*commit_out));

    char *copy = malloc(len + 1);
    if (!copy) return -1;

    memcpy(copy, data, len);
    copy[len] = '\0';

    char *saveptr;
    char *line = strtok_r(copy, "\n", &saveptr);

    while (line) {
        if (strncmp(line, "tree ", 5) == 0) {
            hex_to_hash(line + 5, &commit_out->tree);
        } else if (strncmp(line, "parent ", 7) == 0) {
            hex_to_hash(line + 7, &commit_out->parent);
            commit_out->has_parent = 1;
        } else if (strncmp(line, "author ", 7) == 0) {
            char *last_space = strrchr(line + 7, ' ');
            if (last_space) {
                *last_space = '\0';
                strncpy(commit_out->author, line + 7, sizeof(commit_out->author) - 1);
                commit_out->timestamp = strtoull(last_space + 1, NULL, 10);
            }
        } else if (line[0] == '\0') {
            char *msg = saveptr ? saveptr : "";
            strncpy(commit_out->message, msg, sizeof(commit_out->message) - 1);
            break;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);
    return 0;
}

// Serialize Commit into text format.
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    char parent_hex[HASH_HEX_SIZE + 1];

    hash_to_hex(&commit->tree, tree_hex);
    hash_to_hex(&commit->parent, parent_hex);

    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf) return -1;

    int n = 0;

    n += snprintf(buf + n, cap - n, "tree %s\n", tree_hex);

    if (commit->has_parent) {
        n += snprintf(buf + n, cap - n, "parent %s\n", parent_hex);
    }

    n += snprintf(buf + n, cap - n, "author %s %" PRIu64 "\n",
                  commit->author, commit->timestamp);

    n += snprintf(buf + n, cap - n, "\n%s\n", commit->message);

    *data_out = buf;
    *len_out = (size_t)n;
    return 0;
}

// Read current HEAD commit hash.
int head_read(ObjectID *id_out) {
    FILE *fp = fopen(".pes/refs/heads/main", "r");
    if (!fp) return -1;

    char hex[HASH_HEX_SIZE + 1];
    if (!fgets(hex, sizeof(hex), fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    hex[strcspn(hex, "\r\n")] = '\0';
    return hex_to_hash(hex, id_out);
}

// Update HEAD branch pointer.
int head_update(const ObjectID *id) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);

    FILE *fp = fopen(".pes/refs/heads/main", "w");
    if (!fp) return -1;

    fprintf(fp, "%s\n", hex);
    fclose(fp);
    return 0;
}

// Walk commit history from HEAD backward.
int commit_walk(commit_walk_fn callback) {
    ObjectID current;

    if (head_read(&current) != 0)
        return -1;

    while (1) {
        ObjectType type;
        void *data = NULL;
        size_t len = 0;

        if (object_read(&current, &type, &data, &len) != 0)
            return -1;

        if (type != OBJ_COMMIT) {
            free(data);
            return -1;
        }

        Commit c;
        commit_parse(data, len, &c);
        free(data);

        callback(&current, &c);

        if (!c.has_parent)
            break;

        current = c.parent;
    }

    return 0;
}

// ─── TODO IMPLEMENTATION ──────────────────────────────────────────

// Create a commit from current staged state.
int commit_create(const char *message, ObjectID *commit_id_out) {
    ObjectID tree_id;

    // Build snapshot tree
    if (tree_from_index(&tree_id) != 0)
        return -1;

    Commit commit;
    memset(&commit, 0, sizeof(commit));

    commit.tree = tree_id;

    // Read parent commit if exists
    ObjectID parent_id;
    if (head_read(&parent_id) == 0) {
        commit.has_parent = 1;
        commit.parent = parent_id;
    } else {
        commit.has_parent = 0;
    }

    // Metadata
    strncpy(commit.author, pes_author(), sizeof(commit.author) - 1);
    commit.timestamp = (uint64_t)time(NULL);
    strncpy(commit.message, message, sizeof(commit.message) - 1);

    // Serialize commit
    void *data = NULL;
    size_t len = 0;

    if (commit_serialize(&commit, &data, &len) != 0)
        return -1;

    // Store commit object
    if (object_write(OBJ_COMMIT, data, len, commit_id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);

    // Update HEAD
    if (head_update(commit_id_out) != 0)
        return -1;

    return 0;
}
