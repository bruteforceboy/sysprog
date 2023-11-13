#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define THREAD_VAL_NEW 0
#define THREAD_VAL_RUNNING 1
#define THREAD_VAL_FINISHED 2

#define max(a, b) (a) > (b) ? (a) : (b)

struct thread_task {
    thread_task_f function;
    void *arg;
    void *result;
    pthread_mutex_t *t_mutex;
    int thread_value;
    pthread_cond_t *t_cond;
    struct thread_task* next_task;
    struct thread_pool* t_pool;
    int detached;
};

struct thread_pool {
    pthread_t *threads;
    pthread_mutex_t *p_mutex;
    pthread_cond_t *p_cond;
    int max_thread_count;
    int pool_thread_count;
    struct thread_task *pool_head;
    struct thread_task *pool_list;
    int task_count;
    int active_count;
    bool pool_alive;
};

static void init_pool(int max_thread_count, struct thread_pool *pool) {
    pool->max_thread_count = max_thread_count;
    pool->pool_thread_count = 0;
    pool->threads = NULL;
    pool->pool_head = NULL;
    pool->pool_list = NULL;
    pool->task_count = 0;
    pool->pool_alive = true;
    pool->p_mutex = malloc(sizeof(pthread_mutex_t));
    pool->p_cond = malloc(sizeof(pthread_cond_t));
    pthread_mutex_init(pool->p_mutex, NULL);
    pthread_cond_init(pool->p_cond, NULL);
}

int thread_pool_new(int max_thread_count, struct thread_pool **pool) {
    if (max_thread_count <= 0)
        return TPOOL_ERR_INVALID_ARGUMENT;
    if (max_thread_count > TPOOL_MAX_THREADS)
        return TPOOL_ERR_INVALID_ARGUMENT;

    struct thread_pool *ret_pool = malloc(sizeof(struct thread_pool));

    if (!ret_pool)
        return -1;

    init_pool(max_thread_count, ret_pool);

    *pool = ret_pool;

    return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool) {
    return pool->pool_thread_count;
}

int thread_pool_delete(struct thread_pool *pool) {
    pthread_mutex_lock(pool->p_mutex);
    if (pool->task_count != 0) {
        pthread_mutex_unlock(pool->p_mutex);
        return TPOOL_ERR_HAS_TASKS;
    }

    pool->pool_alive = false;
    pthread_cond_broadcast(pool->p_cond);

    pthread_mutex_unlock(pool->p_mutex);
    for (int i = 0; i < pool->pool_thread_count; i++)
        pthread_join(pool->threads[i], 0);

    pthread_mutex_destroy(pool->p_mutex);
    pthread_cond_destroy(pool->p_cond);

    free(pool->threads);
    free(pool->p_mutex);
    free(pool->p_cond);
    free(pool);

    return 0;
}

static void add_to_list(struct thread_pool *pool, struct thread_task *task) {
    if (pool->pool_list != NULL) {
        pool->pool_list->next_task = task;
        pool->pool_list = task;
    } else {
        pool->pool_list = task;
        pool->pool_head = task;
    }
}

static void *pool_thread_worker(void *vpool) {
    struct thread_pool *pool = vpool;
    do {
        pthread_mutex_lock(pool->p_mutex);
        if (pool->pool_head == NULL) {
            if (!pool->pool_alive) {
                pthread_mutex_unlock(pool->p_mutex);
                break;
            }
            pthread_cond_wait(pool->p_cond, pool->p_mutex);
            pthread_mutex_unlock(pool->p_mutex);
        } else {
            struct thread_task* cur_task = pool->pool_head;
            if (pool->pool_list == cur_task)
                pool->pool_list = NULL;
            pool->pool_head = cur_task->next_task;
            cur_task->next_task = NULL;
            pthread_mutex_unlock(pool->p_mutex);

            pthread_mutex_lock(cur_task->t_mutex);
            cur_task->thread_value = THREAD_VAL_RUNNING;
            pthread_mutex_unlock(cur_task->t_mutex);

            cur_task->result = cur_task->function(cur_task->arg);

            pthread_mutex_lock(cur_task->t_mutex);
            if (cur_task->detached) {
                pthread_mutex_unlock(cur_task->t_mutex);
                pthread_mutex_lock(pool->p_mutex);
                --pool->task_count;
                pthread_mutex_unlock(pool->p_mutex);
                cur_task->t_pool = NULL;
                thread_task_delete(cur_task);
            } else {
                cur_task->thread_value = THREAD_VAL_FINISHED;
                pthread_cond_signal(cur_task->t_cond);
                pthread_mutex_unlock(cur_task->t_mutex);
            }
        }
    } while (1);

    return 0;
}

int thread_pool_push_task(struct thread_pool *pool, struct thread_task *task) {
    pthread_mutex_lock(task->t_mutex);
    if (task->t_pool && task->thread_value != THREAD_VAL_FINISHED) {
        pthread_mutex_unlock(task->t_mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    pthread_mutex_unlock(task->t_mutex);

    pthread_mutex_lock(pool->p_mutex);
    if (pool->task_count == TPOOL_MAX_TASKS) {
        pthread_mutex_unlock(pool->p_mutex);
        return TPOOL_ERR_TOO_MANY_TASKS;
    }
    pthread_mutex_unlock(pool->p_mutex);

    pthread_mutex_lock(task->t_mutex);
    task->t_pool = pool;
    task->thread_value = THREAD_VAL_NEW;
    pthread_mutex_unlock(task->t_mutex);

    pthread_mutex_lock(pool->p_mutex);
    add_to_list(pool, task);
    ++pool->task_count;

    if (pool->task_count > pool->pool_thread_count &&
            pool->pool_thread_count < pool->max_thread_count) {
        pool->threads = realloc(pool->threads, sizeof(pthread_t) * (pool->pool_thread_count + 1));
        if (pool->threads == NULL)
            return -1;
        pthread_create(&pool->threads[pool->pool_thread_count],
                       NULL,
                       pool_thread_worker, pool);
        ++pool->pool_thread_count;
    }

    pthread_cond_signal(pool->p_cond);
    pthread_mutex_unlock(pool->p_mutex);

    return 0;
}

int thread_task_new(struct thread_task **task, thread_task_f function, void *arg) {
    *task = malloc((size_t) sizeof(struct thread_task));
    if (*task == NULL) return -1;
    (*task)->function = function;
    (*task)->arg = arg;
    (*task)->thread_value = THREAD_VAL_NEW;
    (*task)->t_mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    (*task)->t_cond = (pthread_cond_t *) malloc(sizeof(pthread_cond_t));

    pthread_mutex_init((*task)->t_mutex, NULL);
    (*task)->detached = 0;

    pthread_condattr_t attr;
    pthread_cond_init((*task)->t_cond, &attr);

    (*task)->t_pool = NULL;
    (*task)->next_task = NULL;

    return 0;
}

bool thread_task_is_finished(const struct thread_task *task) {
    pthread_mutex_lock(task->t_mutex);
    bool ret = task->thread_value == THREAD_VAL_FINISHED;
    pthread_mutex_unlock(task->t_mutex);
    return ret;
}

bool thread_task_is_running(const struct thread_task *task) {
    pthread_mutex_lock(task->t_mutex);
    bool ret = task->thread_value == THREAD_VAL_RUNNING;
    pthread_mutex_unlock(task->t_mutex);
    return ret;
}

int thread_task_join(struct thread_task *task, void **result) {
    if (task->t_pool == NULL) return TPOOL_ERR_TASK_NOT_PUSHED;
    pthread_mutex_lock(task->t_mutex);
    while (task->thread_value != THREAD_VAL_FINISHED)
        pthread_cond_wait(task->t_cond, task->t_mutex);
    task->t_pool->task_count -= 1;
    pthread_mutex_unlock(task->t_mutex);
    task->t_pool = NULL;
    if (result) *result = task->result;

    return 0;
}

#ifdef NEED_TIMED_JOIN

int thread_task_timed_join(struct thread_task *task, double timeout, void **result) {
    if (!task->t_pool) return TPOOL_ERR_TASK_NOT_PUSHED;

    struct timespec tm;
    clock_gettime(CLOCK_MONOTONIC, &tm);

    long nsec = max(0, timeout) * 1000000000;
    tm.tv_nsec += nsec;
    tm.tv_sec += tm.tv_nsec / 1000000000;
    tm.tv_nsec %= 1000000000;

    pthread_mutex_lock(task->t_mutex);
    bool failed = false;
    while (task->thread_value != THREAD_VAL_FINISHED) {
        if (pthread_cond_timedwait(task->t_cond, task->t_mutex,
                                   &tm) == ETIMEDOUT) {
            failed = true;
            break;
        }
    }

    pthread_mutex_unlock(task->t_mutex);

    if (failed) 
        return TPOOL_ERR_TIMEOUT;
    
    pthread_mutex_lock(task->t_mutex);
    --task->t_pool->task_count;
    pthread_mutex_unlock(task->t_mutex);

    task->t_pool = NULL;
    if (result) *result = task->result;

    return 0;
}

#endif

int thread_task_delete(struct thread_task *task) {
    if (task->t_pool != NULL)
        return TPOOL_ERR_TASK_IN_POOL;
    pthread_mutex_destroy(task->t_mutex);
    pthread_cond_destroy(task->t_cond);
    free(task->t_mutex);
    free(task->t_cond);
    free(task);
    return 0;
}

#ifdef NEED_DETACH

int thread_task_detach(struct thread_task *task) {
    if (!task->t_pool) return TPOOL_ERR_TASK_NOT_PUSHED;

    pthread_mutex_lock(task->t_mutex);

    if (task->thread_value == THREAD_VAL_FINISHED) {
        pthread_mutex_unlock(task->t_mutex);
        pthread_mutex_lock(task->t_mutex);
        --task->t_pool->task_count;
        pthread_mutex_unlock(task->t_mutex);
        task->t_pool = NULL;
        thread_task_delete(task);
    } else {
        task->detached = 1;
        pthread_mutex_unlock(task->t_mutex);
    }

    return 0;
}

#endif