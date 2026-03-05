#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "caesar.h"   // из задания 1

#ifndef BUF_SIZE
#define BUF_SIZE 8192
#endif

static volatile sig_atomic_t keep_running = 1;

static void on_sigint(int signo) {
    (void)signo;
    keep_running = 0;
}

typedef struct {
    FILE* in;
    FILE* out;

    unsigned char buf[BUF_SIZE];
    size_t len;        
    int has_data;      
    int eof;           

    pthread_mutex_t m;
    pthread_cond_t  can_produce;
    pthread_cond_t  can_consume;
} shared_t;

static void die(const char* msg) {
    perror(msg);
    exit(1);
}

static int timed_wait_ms(pthread_cond_t* c, pthread_mutex_t* m, long ms) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;

    long nsec = ts.tv_nsec + ms * 1000000L;
    ts.tv_sec  += nsec / 1000000000L;
    ts.tv_nsec  = nsec % 1000000000L;

    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == 0 || rc == ETIMEDOUT) return rc;
    return rc;
}

static void* producer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;

    while (keep_running) {
        pthread_mutex_lock(&sh->m);

        while (sh->has_data && keep_running) {
            timed_wait_ms(&sh->can_produce, &sh->m, 200);
        }

        if (!keep_running) {
            pthread_mutex_unlock(&sh->m);
            break;
        }

        pthread_mutex_unlock(&sh->m);

        size_t n = fread(sh->buf, 1, BUF_SIZE, sh->in);
        if (n == 0) {
            if (ferror(sh->in)) {
                keep_running = 0;
            }
            pthread_mutex_lock(&sh->m);
            sh->eof = 1;
            pthread_cond_signal(&sh->can_consume);
            pthread_mutex_unlock(&sh->m);
            break;
        }

        caesar(sh->buf, sh->buf, (int)n);

        pthread_mutex_lock(&sh->m);
        sh->len = n;
        sh->has_data = 1;
        pthread_cond_signal(&sh->can_consume);
        pthread_mutex_unlock(&sh->m);
    }

    pthread_mutex_lock(&sh->m);
    pthread_cond_broadcast(&sh->can_consume);
    pthread_mutex_unlock(&sh->m);

    return NULL;
}

static void* consumer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;

    while (1) {
        pthread_mutex_lock(&sh->m);

        while (!sh->has_data && !sh->eof && keep_running) {
            timed_wait_ms(&sh->can_consume, &sh->m, 200);
        }

        if (!keep_running) {
            pthread_mutex_unlock(&sh->m);
            break;
        }

        if (sh->has_data) {
            size_t n = sh->len;

            unsigned char local[BUF_SIZE];
            memcpy(local, sh->buf, n);

            sh->has_data = 0;
            sh->len = 0;
            pthread_cond_signal(&sh->can_produce);
            pthread_mutex_unlock(&sh->m);

            size_t wr = fwrite(local, 1, n, sh->out);
            if (wr != n) {
                keep_running = 0;
                break;
            }
        } else if (sh->eof) {
            pthread_mutex_unlock(&sh->m);
            break;
        } else {
            pthread_mutex_unlock(&sh->m);
        }
    }

    pthread_mutex_lock(&sh->m);
    pthread_cond_broadcast(&sh->can_produce);
    pthread_mutex_unlock(&sh->m);

    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <input_file> <output_file> <key>\n", argv[0]);
        return 2;
    }

    const char* in_path  = argv[1];
    const char* out_path = argv[2];
    const char* key_str  = argv[3];

    if (access(in_path, F_OK) != 0) {
        fprintf(stderr, "Input file does not exist: %s\n", in_path);
        return 2;
    }

    char* endp = NULL;
    long k = strtol(key_str, &endp, 0);
    if (endp == key_str || *endp != '\0' || k < -128 || k > 255) {
        fprintf(stderr, "Invalid key '%s'. Use number in range -128..255.\n", key_str);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) die("sigaction");

    FILE* in = fopen(in_path, "rb");
    if (!in) die("fopen input");

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fclose(in);
        die("fopen output");
    }

    set_key((char)k);

    shared_t sh;
    memset(&sh, 0, sizeof(sh));
    sh.in = in;
    sh.out = out;

    if (pthread_mutex_init(&sh.m, NULL) != 0) die("pthread_mutex_init");
    if (pthread_cond_init(&sh.can_produce, NULL) != 0) die("pthread_cond_init");
    if (pthread_cond_init(&sh.can_consume, NULL) != 0) die("pthread_cond_init");

    pthread_t prod, cons;

    if (pthread_create(&prod, NULL, producer_thread, &sh) != 0) die("pthread_create producer");
    if (pthread_create(&cons, NULL, consumer_thread, &sh) != 0) die("pthread_create consumer");

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    pthread_cond_destroy(&sh.can_produce);
    pthread_cond_destroy(&sh.can_consume);
    pthread_mutex_destroy(&sh.m);

    fclose(in);
    fclose(out);

    if (!keep_running) {
        printf("Операция прервана пользователем\n");
        return 130; 
    }

    return 0;
}