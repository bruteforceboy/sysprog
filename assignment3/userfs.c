#include "userfs.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>

#define max(a, b) (a) > (b) ? (a) : (b)

enum {
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    /** Block memory. */
    char *memory;
    /** How many bytes are occupied. */
    int occupied;
    /** Next block in the file. */
    struct block *next;
    /** Previous block in the file. */
    struct block *prev;
};

struct file {
    /** Double-linked list of file blocks. */
    struct block *block_list;
    /**
     * Last block in the list above for fast access to the end
     * of file.
     */
    struct block *last_block;
    /** How many file descriptors are opened on the file. */
    int refs;
    /** File name. */
    char *name;
    /** Files are stored in a double-linked list. */
    struct file *next;
    struct file *prev;

    size_t block_count;
    bool deleted;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
    struct file *file;

    bool writeable;
    bool readable;

    struct block* block;
    size_t block_id;
    size_t block_pos;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno() {
    return ufs_error_code;
}

static void resize_file_descriptors() {
    int64_t resized_capacity = max(32LL, 2LL * file_descriptor_capacity);
    struct filedesc** resized_fd = realloc(file_descriptors, sizeof(struct filedesc*)
                                           * resized_capacity);
    if (!resized_fd) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return;
    }
    for (int i = file_descriptor_capacity; i < resized_capacity; i++)
        resized_fd[i] = NULL;
    file_descriptors = resized_fd;
    file_descriptor_capacity = resized_capacity;
}

static int get_empty_file_descriptor() {
    if (file_descriptor_count == file_descriptor_capacity)
        resize_file_descriptors();
    if (ufs_error_code != UFS_ERR_NO_ERR)
        return -1;
    int empty_fd = 0;
    while (file_descriptors[empty_fd] != NULL)
        ++empty_fd;
    return empty_fd;
}

static void allocate_block(struct filedesc* fd_ptr) {
    struct block* allocated_block = malloc(sizeof(struct block));
    if (allocated_block == NULL ||
            MAX_FILE_SIZE <= (int64_t) fd_ptr->file->block_count * BLOCK_SIZE) {
        free(allocated_block);
        ufs_error_code = UFS_ERR_NO_MEM;
        return;
    }

    allocated_block->memory = (char*) malloc(BLOCK_SIZE);
    allocated_block->occupied = 0;
    allocated_block->next = NULL;
    allocated_block->prev = fd_ptr->file->last_block;
    if (fd_ptr->file->last_block != NULL)
        fd_ptr->file->last_block->next = allocated_block;
    fd_ptr->file->last_block = allocated_block;
    if (fd_ptr->file->block_list == NULL) {
        fd_ptr->file->block_list = allocated_block;
    }
    ++fd_ptr->file->block_count;

    if (fd_ptr->file->block_count > 1)
        ++fd_ptr->block_id;
    fd_ptr->block_pos = 0;
    fd_ptr->block = fd_ptr->file->last_block;
    assert(fd_ptr->block->memory != NULL);
}

static void adjust_block_position(struct filedesc* fd_ptr) {
    if (fd_ptr->file->block_count != 0) {
        if (fd_ptr->block_id >= fd_ptr->file->block_count) {
            fd_ptr->block_id = fd_ptr->file->block_count - 1;
            fd_ptr->block = fd_ptr->file->last_block;
        }
        if ((int) fd_ptr->block_pos > fd_ptr->block->occupied)
            fd_ptr->block_pos = fd_ptr->block->occupied;
    } else {
        fd_ptr->block_id = 0;
        fd_ptr->block_pos = 0;
        fd_ptr->block = NULL;
    }
}

static void cleanup_blocks(struct file *file, size_t num_blocks) {
    size_t cleaned;

    for (cleaned = 0; file->last_block != NULL
            && cleaned < num_blocks; ++cleaned) {
        struct block* prev_block = file->last_block->prev;
        --file->block_count;
        free(file->last_block->memory);
        free(file->last_block);
        file->last_block = prev_block;
        if (prev_block) prev_block->next = NULL;
        else file->block_list = NULL;
    }
}

static void remove_file_from_list(struct file *file) {
    file_list = (file == file_list ? file->next : file_list);
    if (file->next != NULL) {
        file->next->prev = file->prev;
        file->next = NULL;
    }
    if (file->prev != NULL) {
        file->prev->next = file->next;
        file->prev = NULL;
    }
    if (file->refs == 0) {
        cleanup_blocks(file, file->block_count);
        free(file->name);
        free(file);
    }
}

static void free_file_descriptor(int fd) {
    struct filedesc *fd_ptr = file_descriptors[fd];

    if (fd_ptr == NULL) return;
    struct file *file = fd_ptr->file;

    --file->refs;

    if (file->refs == 0 && file->deleted) {
        remove_file_from_list(file);
    }

    --file_descriptor_count;
    free(file_descriptors[fd]);
    file_descriptors[fd] = NULL;
}

static void clear_file_list() {
    while (1) {
        if (file_list == NULL)
            break;
        remove_file_from_list(file_list);
    }
    free(file_list);
}

int ufs_open(const char *filename, int flags) {
    ufs_error_code = UFS_ERR_NO_ERR;

    struct file* file_list_head = file_list;
    while (file_list_head != NULL && strcmp(file_list_head->name, filename) != 0)
        file_list_head = file_list_head->next;

    if (file_list_head == NULL) {
        if ((flags & UFS_CREATE) != 0) {
            struct file* file_ptr = malloc(sizeof(struct file));
            if (file_ptr == NULL) {
                free(file_ptr);
                ufs_error_code = UFS_ERR_NO_MEM;
                return -1;
            }

            file_ptr->block_list = NULL;
            file_ptr->last_block = NULL;
            file_ptr->refs = 0;
            file_ptr->name = (char *) malloc(strlen(filename) + 1);
            strcpy(file_ptr->name, filename);
            file_ptr->deleted = false;
            file_ptr->block_count = 0;
            file_ptr->next = file_list;
            file_ptr->prev = NULL;
            if (file_list != NULL)
                file_list->prev = file_ptr;
            file_list = file_ptr;
            file_list_head = file_ptr;
        } else {
            ufs_error_code = UFS_ERR_NO_FILE;
            return -1;
        }
    }

    int empty_fd = get_empty_file_descriptor();
    if (ufs_error_code != UFS_ERR_NO_ERR) {
        free(file_list->name);
        free(file_list);
        return -1;
    }

    int permission_flags = flags & (UFS_READ_ONLY | UFS_WRITE_ONLY | UFS_READ_WRITE);
    bool readable, writeable;
    if (permission_flags == UFS_READ_ONLY) readable = true;
    else if (permission_flags == UFS_WRITE_ONLY) writeable = true;
    else if (permission_flags == 0 || permission_flags == UFS_READ_WRITE) {
        readable = true;
        writeable = true;
    } else {
        free(file_list->name);
        free(file_list);
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }

    struct filedesc* fd_ptr = malloc(sizeof(struct filedesc));
    if (fd_ptr == NULL) {
        free(file_list->name);
        free(file_list);
        free(fd_ptr);
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    fd_ptr->block = file_list_head->block_list;
    fd_ptr->block_id = 0;
    fd_ptr->block_pos = 0;
    fd_ptr->file = file_list_head;
    fd_ptr->readable = readable;
    fd_ptr->writeable = writeable;

    file_descriptors[empty_fd] = fd_ptr;
    ++file_list_head->refs;
    ++file_descriptor_count;

    return empty_fd;
}

static void copy_to_current_block(struct filedesc* fd_ptr, const char* buf, size_t* write_total, size_t size) {
    size_t remaining_space = BLOCK_SIZE - fd_ptr->block_pos;
    size_t cur_writing = (remaining_space <= size - *write_total) ? remaining_space : size - *write_total;
    memcpy(fd_ptr->block->memory + fd_ptr->block_pos, buf + *write_total, cur_writing);
    *write_total += cur_writing;
    fd_ptr->block_pos += cur_writing;
    if (fd_ptr->block->occupied < (int) fd_ptr->block_pos)
        fd_ptr->block->occupied = fd_ptr->block_pos;
}

ssize_t ufs_write(int fd, const char *buf, size_t size) {
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd >= file_descriptor_capacity || fd < 0 || file_descriptors[fd] == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (size == 0) return 0;

    struct filedesc* fd_ptr = file_descriptors[fd];
    if (fd_ptr == NULL)
        return -1;
    if (!fd_ptr->writeable) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }
    if (fd_ptr->block == NULL) {
        if (fd_ptr->file->block_list != NULL) {
            fd_ptr->block = fd_ptr->file->block_list;
        } else {
            allocate_block(fd_ptr);
            if (ufs_error_code != UFS_ERR_NO_ERR)
                return -1;
        }
    }

    size_t write_total = 0;
    adjust_block_position(fd_ptr);

    while (write_total < size) {
        copy_to_current_block(fd_ptr, buf, &write_total, size);
        if (write_total == size) break;

        if (fd_ptr->block->next != NULL) {
            fd_ptr->block = fd_ptr->block->next;
            fd_ptr->block_pos = 0;
        } else {
            allocate_block(fd_ptr);
            if (ufs_error_code != UFS_ERR_NO_ERR)
                break;
        }
    }

    if (write_total == 0)
        return -1;

    return write_total;
}

static void copy_from_current_block(struct filedesc* fd_ptr, char* buf, size_t* read_total, size_t size) {
    size_t remaining_data = fd_ptr->block->occupied - fd_ptr->block_pos;
    size_t read_size = (remaining_data < size - *read_total) ? remaining_data : size - *read_total;
    memcpy(buf + *read_total, fd_ptr->block->memory + fd_ptr->block_pos, read_size);
    *read_total += read_size;
    fd_ptr->block_pos += read_size;
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd >= file_descriptor_capacity || fd < 0 || file_descriptors[fd] == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (size == 0) return 0;

    struct filedesc *fd_ptr = file_descriptors[fd];
    if (fd_ptr == NULL) return -1;
    if (!fd_ptr->readable) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }
    if (fd_ptr->block == NULL && fd_ptr->file->block_list == NULL)
        return 0;

    if (fd_ptr->block == NULL)
        fd_ptr->block = fd_ptr->file->block_list;

    size_t read_total = 0;
    adjust_block_position(fd_ptr);

    while (read_total < size) {
        copy_from_current_block(fd_ptr, buf, &read_total, size);
        if (read_total == size || fd_ptr->block->next == NULL) break;
        fd_ptr->block_pos = 0;
        fd_ptr->block = fd_ptr->block->next;
    }

    return read_total;
}

int ufs_close(int fd) {
    if (fd >= file_descriptor_capacity || fd < 0 || file_descriptors[fd] == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    ufs_error_code = UFS_ERR_NO_ERR;
    struct filedesc *fd_ptr = file_descriptors[fd];
    if (fd_ptr == NULL) return -1;
    free_file_descriptor(fd);
    return 0;
}

int ufs_delete(const char* filename) {
    ufs_error_code = UFS_ERR_NO_ERR;

    struct file* file_list_head = file_list;
    while (file_list_head != NULL && strcmp(file_list_head->name, filename) != 0)
        file_list_head = file_list_head->next;

    if (file_list_head == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    file_list_head->deleted = true;
    remove_file_from_list(file_list_head);
    return 0;
}

void ufs_destroy() {
    int file_id = 0;
    while (file_id < file_descriptor_capacity) {
        if (file_descriptors[file_id] != NULL) free_file_descriptor(file_id);
        file_id++;
    }

    clear_file_list();

    free(file_descriptors);
}

int ufs_resize(int fd, size_t new_size) {
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd >= file_descriptor_capacity || fd < 0 || file_descriptors[fd] == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    if (MAX_FILE_SIZE < new_size) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }
    struct filedesc *fd_ptr = file_descriptors[fd];
    if (fd_ptr == NULL) return -1;

    if (!fd_ptr->writeable) {
        free(fd_ptr);
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }

    size_t cur_block_count = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    if (cur_block_count <= fd_ptr->file->block_count) {
        size_t cnt = fd_ptr->file->block_count - cur_block_count;
        if (cnt > 0) {
            cleanup_blocks(fd_ptr->file, cnt);
            adjust_block_position(fd_ptr);
        }
    } else {
        size_t cnt = cur_block_count - fd_ptr->file->block_count;
        while (cnt > 0) {
            allocate_block(fd_ptr);
            if (ufs_error_code != UFS_ERR_NO_ERR)
                return -1;
            --cnt;
        }
    }

    if (ufs_error_code != UFS_ERR_NO_ERR)
        return -1;

    if (fd_ptr->block != NULL) {
        fd_ptr->block->occupied = new_size % BLOCK_SIZE;
        if (fd_ptr->block->occupied == 0)
            fd_ptr->block->occupied = BLOCK_SIZE;
    }

    return 0;
}