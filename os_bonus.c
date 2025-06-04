#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <signal.h>
#include <errno.h>

#define SEM_PARENT 0
#define SEM_CHILD 1

#define MESSAGE_TYPE_PARENT_TO_CHILD 1
#define MESSAGE_TYPE_CHILD_TO_PARENT 2

typedef struct {
    int type;
    char text[128];
} Message;

int pipe_fd[2];
int semaphore_id;
pid_t child_pid;
volatile sig_atomic_t terminate_flag = 0;

void perform_semaphore_operation(int sem_num, int op) {
    struct sembuf sb = {sem_num, op, 0};
    while (semop(semaphore_id, &sb, 1) == -1 && errno == EINTR);
}

void handle_termination_signal(int sig) {
    printf("\nПолучен сигнал завершения (SIGINT)\n");
    terminate_flag = 1;
}

void cleanup_resources(int is_parent) {
    if (is_parent) {
        printf("[Родитель] Начинаю очистку ресурсов...\n");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        if (semctl(semaphore_id, 0, IPC_RMID) == -1) {
            perror("[Родитель] Ошибка при удалении семафора");
        } else {
            printf("[Родитель] Семафор успешно удален\n");
        }
        printf("[Родитель] Ресурсы очищены, завершаю работу.\n");
    } else {
        printf("[Потомок] Начинаю очистку ресурсов...\n");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        printf("[Потомок] Ресурсы очищены, завершаю работу.\n");
    }
}

void child_process_logic() {
    Message message;
    int message_count = 0;
    printf("[Потомок] Процесс запущен\n");
    while (!terminate_flag) {
        perform_semaphore_operation(SEM_CHILD, -1);
        ssize_t r = read(pipe_fd[0], &message, sizeof(Message));
        if (r <= 0 || terminate_flag) break;
        if (message.type == MESSAGE_TYPE_PARENT_TO_CHILD) {
            printf("[Потомок] Получено: %s\n", message.text);
        }
        if (terminate_flag) break;
        sleep(1);
        message.type = MESSAGE_TYPE_CHILD_TO_PARENT;
        snprintf(message.text, sizeof(message.text), "Сообщение от дочернего %d", message_count + 1);
        write(pipe_fd[1], &message, sizeof(Message));
        printf("[Потомок] Отправлено: %s\n", message.text);
        perform_semaphore_operation(SEM_PARENT, 1);
        message_count++;
    }
    printf("[Потомок] Получен сигнал завершения, начинаю завершение работы...\n");
    cleanup_resources(0);
    exit(0);
}

void parent_process_logic() {
    Message message;
    int message_count = 0;
    printf("[Родитель] Процесс запущен\n");
    while (!terminate_flag) {
        sleep(1);
        message.type = MESSAGE_TYPE_PARENT_TO_CHILD;
        snprintf(message.text, sizeof(message.text), "Сообщение от родителя %d", message_count + 1);
        write(pipe_fd[1], &message, sizeof(Message));
        printf("[Родитель] Отправлено: %s\n", message.text);
        perform_semaphore_operation(SEM_CHILD, 1);
        perform_semaphore_operation(SEM_PARENT, -1);
        ssize_t r = read(pipe_fd[0], &message, sizeof(Message));
        if (r <= 0 || terminate_flag) break;
        if (message.type == MESSAGE_TYPE_CHILD_TO_PARENT) {
            printf("[Родитель] Получено: %s\n", message.text);
        }
        message_count++;
    }
    printf("[Родитель] Получен сигнал завершения, начинаю завершение работы...\n");
    if (child_pid > 0) {
        printf("[Родитель] Отправляю сигнал завершения потомку (PID: %d)\n", child_pid);
        kill(child_pid, SIGINT);
    }
    wait(NULL);
    printf("[Родитель] Потомок завершил работу\n");
    cleanup_resources(1);
    printf("[Родитель] Программа успешно завершена\n");
}

int main() {
    if (pipe(pipe_fd) == -1) exit(EXIT_FAILURE);
    semaphore_id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0666);
    if (semaphore_id == -1) exit(EXIT_FAILURE);
    if (semctl(semaphore_id, SEM_PARENT, SETVAL, 0) == -1) exit(EXIT_FAILURE);
    if (semctl(semaphore_id, SEM_CHILD, SETVAL, 0) == -1) exit(EXIT_FAILURE);
    if (signal(SIGINT, handle_termination_signal) == SIG_ERR) exit(EXIT_FAILURE);
    child_pid = fork();
    if (child_pid < 0) exit(EXIT_FAILURE);
    if (child_pid == 0) {
        child_process_logic();
    } else {
        parent_process_logic();
    }
    return 0;
}