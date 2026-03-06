#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "caesar.h"

#define BUFFER_SIZE 4096

typedef struct queue_node { //информация о структуре узла очереди
    unsigned char* data;
    int size;
    struct queue_node* next;
} queue_node;

typedef struct { //информация о структуре очереди
    queue_node* head;
    queue_node* tail;
    int count;

    pthread_mutex_t mutex;
    pthread_cond_t cond_empty;
} queue;

void queue_init(queue* q) { //инициализация очереди
    q->head = NULL; //установка указателя на голову очереди в NULL
    q->tail = NULL; //установка указателя на хвост очереди в NULL
    q->count = 0; //установка счетчика элементов в очереди в 0
    pthread_mutex_init(&q->mutex, NULL);  //инициализация мьютекса для синхронизации доступа к очереди
    pthread_cond_init(&q->cond_empty, NULL); //мьютекс и условная переменная для синхронизации доступа к очереди
}

void queue_push(queue* q, unsigned char* data, int size) { //добавление элемента в очередь
    queue_node* node = malloc(sizeof(queue_node));
    node->data = malloc(size);
    memcpy(node->data, data, size); //копирование данных в новый узел
    node->size = size;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);  //блокировка мьютекса для синхронизации доступа к очереди

    if (q->tail == NULL) {
        q->head = q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }
    q->count++;

    pthread_cond_signal(&q->cond_empty);
    pthread_mutex_unlock(&q->mutex);
}

unsigned char* queue_pop(queue* q, int* size) { //удаление элемента из очереди и возвращение его данных и размера
    pthread_mutex_lock(&q->mutex);

    while (q->head == NULL) {
        pthread_cond_wait(&q->cond_empty, &q->mutex);
    }

    queue_node* node = q->head;
    unsigned char* data = node->data;
    *size = node->size;

    q->head = node->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    q->count--;

    free(node);

    pthread_mutex_unlock(&q->mutex);
    return data;
}

void queue_destroy(queue* q) { //освобождение ресурсов, связанных с очередью
    pthread_mutex_lock(&q->mutex);

    queue_node* current = q->head;
    while (current != NULL) {
        queue_node* next = current->next;
        free(current->data);
        free(current);
        current = next;
    }

    q->head = NULL;
    q->tail = NULL;
    q->count = 0;

    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond_empty);
}
 //глобальные переменные для управления состоянием программы и хранения текущей очереди
volatile int keep_running = 1;
queue current_queue;
volatile char* output_path_to_remove = NULL;

typedef struct { //структура для хранения общих данных между потоками
    FILE* input;
    FILE* output;
    queue q;
    int finished;

    pthread_mutex_t mutex;
} shared_data;

void handle_sigint(int sig) { //обработчик сигнала для прерывания программы
    (void)sig;

    keep_running = 0;

    printf("\nОперация прервана пользователем\n");

    queue_destroy(&current_queue);

    if (output_path_to_remove != NULL) {
        remove(output_path_to_remove);
    }

    exit(1);
}

void* producer(void* arg) { //поток производителя, который читает данные из входного файла, шифрует их и помещает в очередь

    shared_data* data = (shared_data*)arg;

    unsigned char temp[BUFFER_SIZE];

    while (keep_running) {

        int bytes = fread(temp, 1, BUFFER_SIZE, data->input);

        if (bytes <= 0) {
            pthread_mutex_lock(&data->mutex);
            data->finished = 1;
            pthread_mutex_unlock(&data->mutex);
            break;
        }

        unsigned char* encrypted = malloc(bytes);
        caesar(temp, encrypted, bytes);

        queue_push(&data->q, encrypted, bytes);
        free(encrypted);
    }

    return NULL;
}

void* consumer(void* arg) { //поток потребителя, который извлекает зашифрованные данные из очереди и записывает их в выходной файл

    shared_data* data = (shared_data*)arg;

    while (keep_running) {

        int size;
        unsigned char* chunk = queue_pop(&data->q, &size);

        if (chunk == NULL) {
            pthread_mutex_lock(&data->mutex);
            if (data->finished) {
                pthread_mutex_unlock(&data->mutex);
                break;
            }
            pthread_mutex_unlock(&data->mutex);
        }

        fwrite(chunk, 1, size, data->output);
        free(chunk);

        pthread_mutex_lock(&data->mutex);
        if (data->finished && data->q.count == 0) {
            pthread_mutex_unlock(&data->mutex);
            break;
        }
        pthread_mutex_unlock(&data->mutex);
    }

    return NULL;
}

int main(int argc, char* argv[]) {

    char* input_path = argv[1];
    char* output_path = argv[2];
    output_path_to_remove = output_path;
    char key = argv[3][0];

    FILE* input = fopen(input_path, "rb");


    FILE* output = fopen(output_path, "wb");
    if (!output) {
        printf("Cannot open output file\n");
        fclose(input);
        return 1;
    }
    //установка ключа для шифрования
    set_key(key);
//установка обработчика сигнала для прерывания программы
    signal(SIGINT, handle_sigint);

    shared_data data; //инициализация общей структуры данных для потоков

    data.input = input;
    data.output = output;
    data.finished = 0;
    queue_init(&data.q);
    current_queue = data.q;

    pthread_mutex_init(&data.mutex, NULL);

    pthread_t prod; //создание потоков производителя и потребителя
    pthread_t cons;

    pthread_create(&prod, NULL, producer, &data); //запуск потока производителя
    pthread_create(&cons, NULL, consumer, &data);

    pthread_join(prod, NULL); //ожидание завершения потока производителя
    pthread_join(cons, NULL);

    fclose(input);
    fclose(output);

    pthread_mutex_destroy(&data.mutex); //освобождение ресурсов, связанных с общей структурой данных
    queue_destroy(&data.q);

    if (!keep_running)
        printf("Операция прервана пользователем\n");

    return 0;
}