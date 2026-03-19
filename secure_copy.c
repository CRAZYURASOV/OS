#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define WORKER_COUNT 3
#define LOCK_TIMEOUT_SEC 5

static volatile sig_atomic_t keep_running = 1;

typedef void (*set_key_fn)(char);
typedef void (*caesar_fn)(void*, void*, int);

typedef struct {
    char **input_files;
    int total_files;
    int next_index;
    int copied_count;
    const char *output_dir;
    FILE *log_file;
    pthread_mutex_t mutex;

    void *lib_handle;
    caesar_fn caesar;
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
                      long elapsed_ms,
                      const char *details) {
    if (timed_lock_with_warning(&state->mutex, thread_no) != 0) {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));

    fprintf(state->log_file,
            "[%s] pid=%ld thread=%d tid=%lu file=%s result=%s time_ms=%ld",
            timestamp,
            (long)getpid(),
            thread_no,
            (unsigned long)pthread_self(),
            file_name,
            result,
            elapsed_ms);

    if (details != NULL && details[0] != '\0') {
        fprintf(state->log_file, " details=%s", details);
    }

    fprintf(state->log_file, "\n");
    fflush(state->log_file);

    pthread_mutex_unlock(&state->mutex);
}

static int copy_and_encrypt_file(const char *input_path,
                                 const char *output_dir,
                                 caesar_fn caesar,
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

        if (state->next_index < state->total_files) {
            input_path = state->input_files[state->next_index];
            state->next_index++;
        }

        pthread_mutex_unlock(&state->mutex);

        if (input_path == NULL) {
            break;
        }

        struct timespec start_ts, end_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

        char errbuf[256] = {0};
        int rc = copy_and_encrypt_file(input_path,
                                       state->output_dir,
                                       state->caesar,
                                       errbuf,
                                       sizeof(errbuf));

        clock_gettime(CLOCK_MONOTONIC, &end_ts);

        long elapsed_ms =
            (end_ts.tv_sec - start_ts.tv_sec) * 1000L +
            (end_ts.tv_nsec - start_ts.tv_nsec) / 1000000L;

        if (rc == 0) {
            if (timed_lock_with_warning(&state->mutex, thread_no) == 0) {
                state->copied_count++;
                pthread_mutex_unlock(&state->mutex);
            }

            write_log(state, thread_no, base_name(input_path), "success", elapsed_ms, "");
        } else {
            write_log(state, thread_no, base_name(input_path), "error", elapsed_ms, errbuf);
        }
    }

    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Использование: %s <path_to_so> <file1> [file2 ...] <output_dir> <key>\n",
            prog);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        print_usage(argv[0]);
        return 2;
    }

    const char *so_path = argv[1];
    int input_count = argc - 4;
    char **input_files = &argv[2];
    const char *output_dir = argv[argc - 2];
    const char *key_str = argv[argc - 1];

    char *endp = NULL;
    long key_long = strtol(key_str, &endp, 0);
    if (endp == key_str || *endp != '\0' || key_long < -128 || key_long > 255) {
        fprintf(stderr, "Некорректный ключ: %s\n", key_str);
        return 2;
    }

    if (ensure_output_dir(output_dir) != 0) {
        return 1;
    }

    FILE *log_file = fopen("log.txt", "a");
    if (!log_file) {
        perror("fopen log.txt");
        return 1;
    }

    void *handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        fclose(log_file);
        return 1;
    }

    dlerror();
    set_key_fn set_key = (set_key_fn)dlsym(handle, "set_key");
    const char *err1 = dlerror();

    dlerror();
    caesar_fn caesar = (caesar_fn)dlsym(handle, "caesar");
    const char *err2 = dlerror();

    if (err1 || err2 || !set_key || !caesar) {
        fprintf(stderr, "dlsym failed: %s %s\n", err1 ? err1 : "", err2 ? err2 : "");
        dlclose(handle);
        fclose(log_file);
        return 1;
    }

    set_key((char)key_long);

    signal(SIGINT, handle_sigint);

    shared_state state;
    state.input_files = input_files;
    state.total_files = input_count;
    state.next_index = 0;
    state.copied_count = 0;
    state.output_dir = output_dir;
    state.log_file = log_file;
    state.lib_handle = handle;
    state.caesar = caesar;
    pthread_mutex_init(&state.mutex, NULL);

    pthread_t workers[WORKER_COUNT];
    worker_arg args[WORKER_COUNT];

    for (int i = 0; i < WORKER_COUNT; i++) {
        args[i].state = &state;
        args[i].thread_no = i + 1;

        if (pthread_create(&workers[i], NULL, worker_thread, &args[i]) != 0) {
            perror("pthread_create");
            keep_running = 0;

            for (int j = 0; j < i; j++) {
                pthread_join(workers[j], NULL);
            }

            pthread_mutex_destroy(&state.mutex);
            dlclose(handle);
            fclose(log_file);
            return 1;
        }
    }

    for (int i = 0; i < WORKER_COUNT; i++) {
        pthread_join(workers[i], NULL);
    }

    printf("Скопировано файлов: %d из %d\n", state.copied_count, state.total_files);

    pthread_mutex_destroy(&state.mutex);
    dlclose(handle);
    fclose(log_file);

    return (state.copied_count == state.total_files) ? 0 : 1;
}