#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "caesar.h"

#define BUFFER_SIZE 4096
#define WORKER_COUNT 4
#define LOCK_TIMEOUT_SEC 5

static volatile sig_atomic_t keep_running = 1;

typedef enum {
    MODE_AUTO,
    MODE_SEQUENTIAL,
    MODE_PARALLEL,
} run_mode;

typedef struct {
    char **input_files;
    int total_files;
    int next_index;
    int copied_count;
    int processed_files;
    long long total_elapsed_ns;
    const char *output_dir;
    FILE *log_file;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    char **queue;
    int queue_head;
    int queue_tail;
    int queue_size;
    int queue_capacity;
    int done_adding;
} shared_state;

typedef struct {
    shared_state *state;
    int thread_no;
} worker_arg;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int ensure_output_dir(const char *path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        fprintf(stderr, "Ошибка: %s существует, но это не директория\n", path);
        return -1;
    }

    if (mkdir(path, 0777) != 0) {
        perror("mkdir");
        return -1;
    }

    return 0;
}

static void format_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

static const char *mode_name(run_mode mode) {
    switch (mode) {
        case MODE_SEQUENTIAL:
            return "sequential";
        case MODE_PARALLEL:
            return "parallel";
        default:
            return "auto";
    }
}

static int queue_enqueue(shared_state *state, const char *path) {
    if (state->queue_size >= state->queue_capacity) {
        return -1;
    }
    state->queue[state->queue_tail] = (char *)path;
    state->queue_tail = (state->queue_tail + 1) % state->queue_capacity;
    state->queue_size++;
    return 0;
}

static char *queue_dequeue(shared_state *state) {
    if (state->queue_size == 0) {
        return NULL;
    }
    char *path = state->queue[state->queue_head];
    state->queue_head = (state->queue_head + 1) % state->queue_capacity;
    state->queue_size--;
    return path;
}

static int queue_is_empty(shared_state *state) {
    return state->queue_size == 0;
}

static void print_mode_comparison(int total_files, run_mode chosen) {
    run_mode alternative = (chosen == MODE_SEQUENTIAL) ? MODE_PARALLEL : MODE_SEQUENTIAL;
    printf("Автоматический режим: %s. Альтернативный режим: %s.\n",
           mode_name(chosen), mode_name(alternative));
    if (chosen == MODE_SEQUENTIAL) {
        printf("Для %d файлов sequential обычно выгоднее, т.к. параллельный режим накладнее.\n", total_files);
    } else {
        printf("Для %d файлов parallel обычно выгоднее за счет пула потоков.\n", total_files);
    }
}

static int timed_lock_with_warning(pthread_mutex_t *mutex, int thread_no) {
    while (keep_running) {
        struct timespec ts;

        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            perror("clock_gettime");
            return -1;
        }

        ts.tv_sec += LOCK_TIMEOUT_SEC;

        int rc = pthread_mutex_timedlock(mutex, &ts);

        if (rc == 0) {
            return 0;
        }

        if (rc == ETIMEDOUT) {
            fprintf(stderr,
                    "Предупреждение: поток %d ждет мьютекс больше %d секунд\n",
                    thread_no, LOCK_TIMEOUT_SEC);
            continue;
        }

        errno = rc;
        perror("pthread_mutex_timedlock");
        return -1;
    }

    return -1;
}

static void write_log(shared_state *state,
                      int thread_no,
                      const char *file_name,
                      const char *result,
                      long long start_ns,
                      long long end_ns,
                      long elapsed_ms,
                      const char *details) {
    if (timed_lock_with_warning(&state->mutex, thread_no) != 0) {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));

    fprintf(state->log_file,
            "[%s] thread=%d tid=%lu file=%s result=%s start_ns=%lld end_ns=%lld time_ms=%ld",
            timestamp,
            thread_no,
            (unsigned long)pthread_self(),
            file_name,
            result,
            start_ns,
            end_ns,
            elapsed_ms);

    if (details != NULL && details[0] != '\0') {
        fprintf(state->log_file, " details=%s", details);
    }

    fprintf(state->log_file, "\n");
    fflush(state->log_file);

    pthread_mutex_unlock(&state->mutex);
}

static int run_sequential(shared_state *state) {
    for (int i = 0; i < state->total_files && keep_running; i++) {
        const char *input_path = state->input_files[i];
        struct timespec start_ts, end_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

        char errbuf[256] = {0};
        int rc = copy_and_encrypt_file(input_path, state->output_dir, errbuf, sizeof(errbuf));

        clock_gettime(CLOCK_MONOTONIC, &end_ts);
        long long elapsed_ns =
            (end_ts.tv_sec - start_ts.tv_sec) * 1000000000LL +
            (end_ts.tv_nsec - start_ts.tv_nsec);
        long elapsed_ms = (long)(elapsed_ns / 1000000LL);

        state->processed_files++;
        state->total_elapsed_ns += elapsed_ns;
        if (rc == 0) {
            state->copied_count++;
            write_log(state,
                      0,
                      base_name(input_path),
                      "success",
                      start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec,
                      end_ts.tv_sec * 1000000000LL + end_ts.tv_nsec,
                      elapsed_ms,
                      "");
        } else {
            write_log(state,
                      0,
                      base_name(input_path),
                      "error",
                      start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec,
                      end_ts.tv_sec * 1000000000LL + end_ts.tv_nsec,
                      elapsed_ms,
                      errbuf);
        }
    }
    return (state->copied_count == state->total_files) ? 0 : 1;
}

static int copy_and_encrypt_file(const char *input_path,
                                 const char *output_dir,
                                 char *errbuf,
                                 size_t errbuf_size) {
    FILE *in = fopen(input_path, "rb");
    if (!in) {
        snprintf(errbuf, errbuf_size, "cannot open input: %s", strerror(errno));
        return -1;
    }

    const char *name = base_name(input_path);

    size_t out_len = strlen(output_dir) + 1 + strlen(name) + 1;
    char *out_path = (char *)malloc(out_len);
    if (!out_path) {
        fclose(in);
        snprintf(errbuf, errbuf_size, "out of memory");
        return -1;
    }

    snprintf(out_path, out_len, "%s/%s", output_dir, name);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        snprintf(errbuf, errbuf_size, "cannot open output: %s", strerror(errno));
        fclose(in);
        free(out_path);
        return -1;
    }

    unsigned char in_buf[BUFFER_SIZE];
    unsigned char out_buf[BUFFER_SIZE];
    int status = 0;

    while (keep_running) {
        size_t rd = fread(in_buf, 1, sizeof(in_buf), in);

        if (rd > 0) {
            caesar(in_buf, out_buf, (int)rd);

            if (fwrite(out_buf, 1, rd, out) != rd) {
                snprintf(errbuf, errbuf_size, "write error: %s", strerror(errno));
                status = -1;
                break;
            }
        }

        if (rd < sizeof(in_buf)) {
            if (ferror(in)) {
                snprintf(errbuf, errbuf_size, "read error: %s", strerror(errno));
                status = -1;
            }
            break;
        }
    }

    if (!keep_running && status == 0) {
        snprintf(errbuf, errbuf_size, "interrupted");
        status = -1;
    }

    fclose(in);

    if (fclose(out) != 0 && status == 0) {
        snprintf(errbuf, errbuf_size, "close output error: %s", strerror(errno));
        status = -1;
    }

    if (status != 0) {
        remove(out_path);
    }

    free(out_path);
    return status;
}

static void *worker_thread(void *arg) {
    worker_arg *worker = (worker_arg *)arg;
    shared_state *state = worker->state;
    int thread_no = worker->thread_no;

    while (keep_running) {
        char *input_path = NULL;

        if (timed_lock_with_warning(&state->mutex, thread_no) != 0) {
            break;
        }

        while (keep_running && queue_is_empty(state) && !state->done_adding) {
            pthread_cond_wait(&state->cond, &state->mutex);
        }

        if (!keep_running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        if (!queue_is_empty(state)) {
            input_path = queue_dequeue(state);
        } else if (state->done_adding) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        pthread_mutex_unlock(&state->mutex);

        if (input_path == NULL) {
            break;
        }

        struct timespec start_ts, end_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

        char errbuf[256] = {0};
        int rc = copy_and_encrypt_file(input_path, state->output_dir, errbuf, sizeof(errbuf));

        clock_gettime(CLOCK_MONOTONIC, &end_ts);

        long long elapsed_ns =
            (end_ts.tv_sec - start_ts.tv_sec) * 1000000000LL +
            (end_ts.tv_nsec - start_ts.tv_nsec);
        long elapsed_ms = (long)(elapsed_ns / 1000000LL);

        if (timed_lock_with_warning(&state->mutex, thread_no) == 0) {
            state->processed_files++;
            state->total_elapsed_ns += elapsed_ns;
            if (rc == 0) {
                state->copied_count++;
            }
            pthread_mutex_unlock(&state->mutex);
        }

        if (rc == 0) {
            write_log(state,
                      thread_no,
                      base_name(input_path),
                      "success",
                      start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec,
                      end_ts.tv_sec * 1000000000LL + end_ts.tv_nsec,
                      elapsed_ms,
                      "");
        } else {
            write_log(state,
                      thread_no,
                      base_name(input_path),
                      "error",
                      start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec,
                      end_ts.tv_sec * 1000000000LL + end_ts.tv_nsec,
                      elapsed_ms,
                      errbuf);
        }
    }

    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Использование: %s [--mode=sequential|parallel|auto] <file1> [file2 ...] <output_dir> <key>\n",
            prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 2;
    }

    run_mode mode = MODE_AUTO;
    int arg_index = 1;

    if (argc > 1 && strncmp(argv[1], "--mode=", 7) == 0) {
        const char *val = argv[1] + 7;
        if (strcmp(val, "sequential") == 0) {
            mode = MODE_SEQUENTIAL;
        } else if (strcmp(val, "parallel") == 0) {
            mode = MODE_PARALLEL;
        } else if (strcmp(val, "auto") == 0) {
            mode = MODE_AUTO;
        } else {
            fprintf(stderr, "Некорректный режим: %s\n", val);
            print_usage(argv[0]);
            return 2;
        }
        arg_index++;
    }

    int remaining_args = argc - arg_index;
    if (remaining_args < 3) {
        print_usage(argv[0]);
        return 2;
    }

    int input_count = remaining_args - 2;
    char **input_files = &argv[arg_index];
    const char *output_dir = argv[argc - 2];
    const char *key_str = argv[argc - 1];

    char *endp = NULL;
    long key_long = strtol(key_str, &endp, 0);

    if (endp == key_str || *endp != '\0' || key_long < -128 || key_long > 255) {
        fprintf(stderr, "Некорректный ключ: %s\n", key_str);
        return 2;
    }

    set_key((char)key_long);

    if (ensure_output_dir(output_dir) != 0) {
        return 1;
    }

    FILE *log_file = fopen("log.txt", "a");
    if (!log_file) {
        perror("fopen log.txt");
        return 1;
    }

    signal(SIGINT, handle_sigint);

    shared_state state;
    state.input_files = input_files;
    state.total_files = input_count;
    state.next_index = 0;
    state.copied_count = 0;
    state.processed_files = 0;
    state.total_elapsed_ns = 0;
    state.output_dir = output_dir;
    state.log_file = log_file;
    state.queue = NULL;
    state.queue_head = 0;
    state.queue_tail = 0;
    state.queue_size = 0;
    state.queue_capacity = input_count;
    state.done_adding = 0;

    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.cond, NULL);

    run_mode exec_mode = mode;
    if (mode == MODE_AUTO) {
        exec_mode = (input_count < 5) ? MODE_SEQUENTIAL : MODE_PARALLEL;
        print_mode_comparison(input_count, exec_mode);
    }

    struct timespec global_start, global_end;
    clock_gettime(CLOCK_MONOTONIC, &global_start);

    int status = 0;
    pthread_t workers[WORKER_COUNT];
    worker_arg args[WORKER_COUNT];

    if (exec_mode == MODE_PARALLEL) {
        state.queue = malloc(sizeof(char *) * input_count);
        if (!state.queue) {
            perror("malloc");
            status = 1;
            goto cleanup;
        }

        for (int i = 0; i < input_count; i++) {
            if (queue_enqueue(&state, input_files[i]) != 0) {
                fprintf(stderr, "Ошибка: очередь переполнена\n");
                status = 1;
                goto cleanup;
            }
        }
        state.done_adding = 1;
        pthread_cond_broadcast(&state.cond);

        for (int i = 0; i < WORKER_COUNT; i++) {
            args[i].state = &state;
            args[i].thread_no = i + 1;

            if (pthread_create(&workers[i], NULL, worker_thread, &args[i]) != 0) {
                perror("pthread_create");
                keep_running = 0;
                status = 1;
                for (int j = 0; j < i; j++) {
                    pthread_join(workers[j], NULL);
                }
                goto cleanup;
            }
        }

        for (int i = 0; i < WORKER_COUNT; i++) {
            pthread_join(workers[i], NULL);
        }
    } else {
        status = run_sequential(&state);
    }

cleanup:
    clock_gettime(CLOCK_MONOTONIC, &global_end);
    long long total_ns = (global_end.tv_sec - global_start.tv_sec) * 1000000000LL +
                         (global_end.tv_nsec - global_start.tv_nsec);
    double total_sec = total_ns / 1e9;
    double average_ms = state.processed_files > 0 ? (state.total_elapsed_ns / (double)state.processed_files) / 1e6 : 0.0;

    printf("Режим: %s\n", mode_name(exec_mode));
    printf("Скопировано файлов: %d из %d\n", state.copied_count, state.total_files);
    printf("Общее время: %.3f сек\n", total_sec);
    printf("Среднее время на файл: %.3f мс\n", average_ms);
    printf("Всего обработано файлов: %d\n", state.processed_files);

    if (state.queue) {
        free(state.queue);
    }
    pthread_cond_destroy(&state.cond);
    pthread_mutex_destroy(&state.mutex);
    fclose(log_file);

    return status;
}
