#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include "libcoro.h"

struct context_info {
    int coro_id;
    uint64_t total_work_time;
    int num_switches;
    int num_files;
    int *current_file_ptr;
    char **filenames;
    int **sorted_files_array;
    size_t *sorted_files_sizes;
    uint64_t max_time;
    uint64_t prev_start_time;
};

// Reference: https://linux.die.net/man/3/clock_gettime
uint64_t get_time() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) (now.tv_sec * 1000000) + (uint64_t) (now.tv_nsec / 1000);
}

static inline void yield_check(uint64_t* total_work_time, uint64_t* prev_start_time, uint64_t max_time) {
    uint64_t working_time = get_time() - (*prev_start_time);
    if (working_time > max_time) {
        (*total_work_time) += working_time;
        coro_yield();
        *prev_start_time = get_time();
    }
}

static void merge_sort(int* arr, int l, int r, uint64_t* total_work_time, uint64_t* prev_start_time, uint64_t max_time) {
    if (l == r) return;

    int mid = (l + r) / 2;

    yield_check(total_work_time, prev_start_time, max_time);

    merge_sort(arr, l, mid, total_work_time, prev_start_time, max_time);
    merge_sort(arr, mid + 1, r, total_work_time, prev_start_time, max_time);

    int left_len = mid - l + 1;
    int right_len = r - mid;

    int left[left_len];
    int right[right_len];

    for (int i = 0; i < left_len; i++) left[i] = arr[l + i];
    for (int i = 0; i < right_len; i++) right[i] = arr[mid + i + 1];

    int left_idx = 0;
    int right_idx = 0;
    int arr_idx = l;

    while (left_idx < left_len && right_idx < right_len) {
        if (left[left_idx] <= right[right_idx])
            arr[arr_idx++] = left[left_idx++];
        else
            arr[arr_idx++] = right[right_idx++];
    }

    while (left_idx < left_len)
        arr[arr_idx++] = left[left_idx++];

    while (right_idx < right_len)
        arr[arr_idx++] = right[right_idx++];
}

static int coroutine_func_f(void *context) {
    struct coro *this = coro_this();
    struct context_info *ctx = context;
    ctx->prev_start_time = get_time();

    while (*ctx->current_file_ptr < ctx->num_files) {
        int cur_file_idx = *ctx->current_file_ptr;
        ++(*ctx->current_file_ptr);

        FILE* file = fopen(ctx->filenames[cur_file_idx], "r");
        if (file == NULL) {
            printf("Failed to open %s\n", ctx->filenames[cur_file_idx]);
            exit(1);
        }

        int num;
        int arr_len = 0;
        while (fscanf(file, "%d", &num) != EOF) ++arr_len;
        
        int *arr = malloc(sizeof(int) * arr_len);
        fseek(file, 0, SEEK_SET);
        for (int i = 0; i < arr_len; i++) {
            fscanf(file, "%d", &arr[i]);
        }

        yield_check(&ctx->total_work_time, &ctx->prev_start_time, ctx->max_time);        

        ctx->sorted_files_array[cur_file_idx] = arr;
        ctx->sorted_files_sizes[cur_file_idx] = arr_len;

        merge_sort(arr, 0, arr_len - 1, &ctx->total_work_time, &ctx->prev_start_time, ctx->max_time);
        fclose(file);
    }

    ctx->num_switches = coro_switch_count(this);
    ctx->total_work_time += get_time() - ctx->prev_start_time;

    printf("Name: coro_%d\n", ctx->coro_id);
    printf("Total Work Time: %ldμs\n", ctx->total_work_time);
    printf("Number of Context Switches: %d\n", ctx->num_switches);

    return 0;
}

int main(int argc, char **argv) {
    int num_coroutines = 0;
    int target_latency = 0;

    bool argc_error = false;

    int arg_idx = 1;
    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "-n") == 0 && arg_idx + 1 < argc) {
            num_coroutines = atoi(argv[arg_idx + 1]);
            arg_idx += 2;
        } else if (strcmp(argv[arg_idx], "-t") == 0 && arg_idx + 1 < argc) {
            target_latency = atoi(argv[arg_idx + 1]);
            arg_idx += 2;
        } else {
            break;
        }
    }

    if (argc_error || num_coroutines < 1 || target_latency < num_coroutines || arg_idx == argc) {
        printf("Format: %s\n", argv[0]);
        printf("  -n <number of coroutines>: Specify the number of coroutines (must be at least 1).\n");
        printf("  -t <target latency>: Specify the target latency (must be greater than or equal to the number of coroutines).\n");
        printf("  filename1 filename2 ... : List of filenames to process.\n");
        return 1;
    }

    char **filenames = (argv + arg_idx);
    int files_count = argc - arg_idx;
    int current_file_ptr = 0;

    int **sorted_files_array = malloc(sizeof(int *) * files_count);
    size_t *sorted_files_sizes = malloc(sizeof(size_t) * files_count);

    coro_sched_init();
    struct context_info *coroutine_contexts = (struct context_info *) malloc(sizeof(struct context_info) * num_coroutines);
    for (int i = 0; i < num_coroutines; i++) {
        coroutine_contexts[i].coro_id = i + 1;
        coroutine_contexts[i].total_work_time = 0;
        coroutine_contexts[i].num_switches = 0;
        coroutine_contexts[i].num_files = files_count;
        coroutine_contexts[i].current_file_ptr = &current_file_ptr;
        coroutine_contexts[i].filenames = filenames;
        coroutine_contexts[i].sorted_files_array = sorted_files_array;
        coroutine_contexts[i].sorted_files_sizes = sorted_files_sizes;
        coroutine_contexts[i].max_time = (uint64_t) target_latency / num_coroutines;

        coro_new(coroutine_func_f, &coroutine_contexts[i]);
    }

    struct coro *c;
    while ((c = coro_sched_wait()) != NULL) {
        coro_delete(c);
    }
    printf("Couroutines deleted\n");

    free(coroutine_contexts);

    FILE *merged_output = fopen("output.txt", "w");
    if (merged_output == NULL) {
        printf("Failed to create output.txt\n");
        exit(1);
    } 

    size_t total_numbers = 0;
    size_t* ptr = malloc(sizeof(size_t) * files_count);
    for (int i = 0; i < files_count; i++) {
        total_numbers += sorted_files_sizes[i];
        ptr[i] = 0;
    }

    while (total_numbers-- > 0) {
        int current_min_number = INT_MAX;
        int file_idx = -1;
        for (int i = 0; i < files_count; i++) {
            if (ptr[i] == sorted_files_sizes[i]) 
                continue;
            if (sorted_files_array[i][ptr[i]] <= current_min_number) {
                current_min_number = sorted_files_array[i][ptr[i]];
                file_idx = i;
            }
        }

        ++ptr[file_idx];

        fprintf(merged_output, "%d ", current_min_number);
    }

    fclose(merged_output);
    for (int i = 0; i < files_count; i++) {
        free(sorted_files_array[i]);
    }
    free(sorted_files_array);
    free(sorted_files_sizes);
    free(ptr);

    return 0;
}