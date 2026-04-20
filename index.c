// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

// Load the index from INDEX_FILE (.pes/index).
// If the file doesn't exist, initialize an empty index (not an error).
// Format per line: <mode-octal> <64-hex-hash> <mtime> <size> <path>
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // No index file yet = empty staging area, that's fine
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;  // skip blank lines

        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count];

        char hex[HASH_HEX_SIZE + 2];
        unsigned int mode;
        unsigned long long mtime;
        unsigned int size;
        char path[512];

        // Parse: <mode> <hex> <mtime> <size> <path>
        if (sscanf(line, "%o %64s %llu %u %511s",
                   &mode, hex, &mtime, &size, path) != 5) {
            fclose(f);
            return -1;
        }

        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);
    return 0;
}

// Comparator for qsort — sort index entries by path alphabetically
static int compare_entries_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Save the index to INDEX_FILE atomically (temp file + rename).
// Sort entries by path before writing.
int index_save(const Index *index) {
    // Write to a temp file first
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    // Sort a heap-allocated copy of the entries (Index is too large for the stack)
    IndexEntry *sorted = malloc((size_t)index->count * sizeof(IndexEntry));
    if (!sorted) { fclose(f); return -1; }
    memcpy(sorted, index->entries, (size_t)index->count * sizeof(IndexEntry));
    qsort(sorted, (size_t)index->count, sizeof(IndexEntry), compare_entries_by_path);

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &sorted[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                e->size,
                e->path);
    }
    free(sorted);

    // Flush userspace buffers, sync to disk, then rename atomically
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp, INDEX_FILE) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

// Stage a file: read contents → write blob → update index entry.
int index_add(Index *index, const char *path) {
    // Read the file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    if (file_size < 0) { fclose(f); return -1; }

    unsigned char *contents = malloc((size_t)file_size + 1);
    if (!contents) { fclose(f); return -1; }

    size_t nread = fread(contents, 1, (size_t)file_size, f);
    fclose(f);
    if (nread != (size_t)file_size) { free(contents); return -1; }

    // Write as a blob object
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id) != 0) {
        free(contents);
        return -1;
    }
    free(contents);

    // Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode;
    if (S_ISDIR(st.st_mode))       mode = 0040000;
    else if (st.st_mode & S_IXUSR) mode = 0100755;
    else                            mode = 0100644;

    // Update or insert the index entry
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        // Update in place
        existing->mode      = mode;
        existing->hash      = blob_id;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
    } else {
        // Add new entry
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index full\n");
            return -1;
        }
        IndexEntry *e = &index->entries[index->count++];
        e->mode      = mode;
        e->hash      = blob_id;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    // Persist the updated index atomically
    return index_save(index);
}
