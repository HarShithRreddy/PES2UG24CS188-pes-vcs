// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *p = data;
    const uint8_t *end = p + len;

    while (p < end) {
        if (tree_out->count >= MAX_TREE_ENTRIES) return -1;
        TreeEntry *e = &tree_out->entries[tree_out->count];

        // Parse mode (ASCII until space)
        const uint8_t *space = memchr(p, ' ', end - p);
        if (!space) return -1;
        size_t mode_len = space - p;
        if (mode_len >= sizeof(e->mode)) return -1;
        memcpy(e->mode, p, mode_len);
        e->mode[mode_len] = '\0';
        p = space + 1;

        // Parse name (until null byte)
        const uint8_t *null_b = memchr(p, '\0', end - p);
        if (!null_b) return -1;
        size_t name_len = null_b - p;
        if (name_len >= sizeof(e->name)) return -1;
        memcpy(e->name, p, name_len);
        e->name[name_len] = '\0';
        p = null_b + 1;

        // Parse 32-byte raw hash
        if (p + HASH_SIZE > end) return -1;
        memcpy(e->id.hash, p, HASH_SIZE);
        p += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
static int cmp_entries(const void *a, const void *b) {
    return strcmp(((TreeEntry *)a)->name, ((TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Sort a copy
    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), cmp_entries);

    // Calculate total size
    size_t total = 0;
    for (int i = 0; i < sorted.count; i++) {
        total += strlen(sorted.entries[i].mode) + 1   // "100644 "
               + strlen(sorted.entries[i].name) + 1   // "filename\0"
               + HASH_SIZE;                            // 32 raw bytes
    }

    uint8_t *buf = malloc(total);
    if (!buf) return -1;
    uint8_t *p = buf;

    for (int i = 0; i < sorted.count; i++) {
        TreeEntry *e = &sorted.entries[i];
        size_t mode_len = strlen(e->mode);
        size_t name_len = strlen(e->name);

        memcpy(p, e->mode, mode_len); p += mode_len;
        *p++ = ' ';
        memcpy(p, e->name, name_len); p += name_len;
        *p++ = '\0';
        memcpy(p, e->id.hash, HASH_SIZE); p += HASH_SIZE;
    }

    *data_out = buf;
    *len_out = total;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
int tree_from_index(const Index *index, ObjectID *root_id_out) {
    Tree root;
    root.count = 0;

    for (int i = 0; i < index->count; i++) {
        IndexEntry *ie = &index->entries[i];
        TreeEntry *te = &root.entries[root.count++];

        snprintf(te->mode, sizeof(te->mode), "%06o", ie->mode);
        strncpy(te->name, ie->path, sizeof(te->name) - 1);
        te->name[sizeof(te->name) - 1] = '\0';
        te->id = ie->id;
    }

    void *data;
    size_t len;
    if (tree_serialize(&root, &data, &len) != 0) return -1;
    int r = object_write(OBJ_TREE, data, len, root_id_out);
    free(data);
    return r;
}
