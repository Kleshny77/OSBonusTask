#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#define MESSAGE_BUFFER_SIZE 128

#define SEMAPHORE_PARENT 0
#define SEMAPHORE_CHILD 1

// Two pipes for bidirectional communication
int pipe_parent_to_child[2]; // Parent writes to [1], child reads from [0]
int pipe_child_to_parent[2]; // Child writes to [1], parent reads from [0]
int semaphore_id;
pid_t child_process_id;

volatile sig_atomic_t terminate_flag = 0;

void handle_termination_signal(int sig) {
    terminate_flag = 1;
}

void perform_semaphore_operation(int semaphore_index, int operation) {
    struct sembuf sem_op_buffer = {semaphore_index, operation, SEM_UNDO};
    int result = -1;
    while ((result = semop(semaphore_id, &sem_op_buffer, 1)) == -1 && errno == EINTR && !terminate_flag) {
    }
    if (result == -1 && errno != EINTR && !terminate_flag) {
        perror("semaphore operation failed");
        exit(EXIT_FAILURE);
    }
}

void cleanup_resources(int is_parent) {
    // Close all pipe file descriptors in both processes
    close(pipe_parent_to_child[0]);
    close(pipe_parent_to_child[1]);
    close(pipe_child_to_parent[0]);
    close(pipe_child_to_parent[1]);

    if (is_parent) {
        if (semctl(semaphore_id, 0, IPC_RMID) == -1) {
            perror("semaphore removal failed");
        }
        printf("[Родитель] Ресурсы очищены.\n");
    } else {
        printf("[Потомок] Ресурсы очищены.\n");
    }
}

void parent_process_logic() {
    char message_buffer[MESSAGE_BUFFER_SIZE];
    ssize_t bytes_read_written;
    int message_count = 0;

    // Close unused pipe ends in parent
    close(pipe_parent_to_child[0]); // Parent doesn't read from its write pipe
    close(pipe_child_to_parent[1]); // Parent doesn't write to its read pipe

    printf("[Родитель] Запущен, отправляю первое сообщение.\n");

    while (!terminate_flag && message_count < 10) {
        perform_semaphore_operation(SEMAPHORE_PARENT, -1);
        if (terminate_flag) break;

        // Parent prepares and sends message to child via pipe_parent_to_child[1]
        snprintf(message_buffer, MESSAGE_BUFFER_SIZE, "Сообщение от родителя %d", message_count + 1);
        bytes_read_written = write(pipe_parent_to_child[1], message_buffer, strlen(message_buffer) + 1);
        if (bytes_read_written < 0) {
            if (errno == EINTR) continue;
            perror("parent write to pipe failed");
            break;
        }
        printf("[Родитель] Отправлено: %s\n", message_buffer);

        // Parent signals child that a message is available and it's their turn (signals SEMAPHORE_CHILD)
        perform_semaphore_operation(SEMAPHORE_CHILD, 1);

        // --- Parent's Turn to Receive ---
        // Parent reads child's response from pipe_child_to_parent[0]
        bytes_read_written = read(pipe_child_to_parent[0], message_buffer, MESSAGE_BUFFER_SIZE - 1);
        if (bytes_read_written < 0) {
            if (errno == EINTR) continue;
            perror("parent read from pipe failed");
            break;
        } else if (bytes_read_written == 0) {
            printf("[Родитель] Потомок закрыл канал.\n");
            break;
        }
        message_buffer[bytes_read_written] = '\0';
        printf("[Родитель] Получено: %s\n", message_buffer);

        // Simulate work/delay (1 second interval between full exchanges)
        sleep(1);

        message_count++;
    }

    // Close parent's remaining pipe ends before exiting
    close(pipe_parent_to_child[1]);
    close(pipe_child_to_parent[0]);

    if (child_process_id > 0) {
        printf("[Родитель] Отправляю сигнал завершения потомку %d\n", child_process_id);
        kill(child_process_id, SIGINT);
        wait(NULL); // Wait for child to exit
    }

    cleanup_resources(1);
}

void child_process_logic() {
    char message_buffer[MESSAGE_BUFFER_SIZE];
    ssize_t bytes_read_written;
    int message_count = 0;

    // Close unused pipe ends in child
    close(pipe_parent_to_child[1]); // Child doesn't write to its read pipe
    close(pipe_child_to_parent[0]); // Child doesn't read from its write pipe

    printf("[Потомок] Запущен, ожидаю сообщение от родителя.\n");

    while (!terminate_flag && message_count < 10) {
        // --- Child's Turn to Receive ---
        // Child waits for parent to send a message (waits on SEMAPHORE_CHILD)
        perform_semaphore_operation(SEMAPHORE_CHILD, -1);
        if (terminate_flag) break;

        // Child reads message from pipe_parent_to_child[0]
        bytes_read_written = read(pipe_parent_to_child[0], message_buffer, MESSAGE_BUFFER_SIZE - 1);
        if (bytes_read_written < 0) {
            if (errno == EINTR) continue;
            perror("child read from pipe failed");
            break;
        } else if (bytes_read_written == 0) {
            printf("[Потомок] Родитель закрыл канал.\n");
            break;
        }
        message_buffer[bytes_read_written] = '\0';
        printf("[Потомок] Получено: %s\n", message_buffer);

        // Simulate work/delay (part of the exchange interval)
        sleep(1);

        // --- Child's Turn to Send ---
        // Child prepares and sends response to parent via pipe_child_to_parent[1]
        snprintf(message_buffer, MESSAGE_BUFFER_SIZE, "Сообщение от дочернего %d", message_count + 1);
        bytes_read_written = write(pipe_child_to_parent[1], message_buffer, strlen(message_buffer) + 1);
        if (bytes_read_written < 0) {
            if (errno == EINTR) continue;
            perror("child write to pipe failed");
            break;
        }
        printf("[Потомок] Отправлено: %s\n", message_buffer);

        // Child signals parent that a response is available and it's parent's turn (signals SEMAPHORE_PARENT)
        perform_semaphore_operation(SEMAPHORE_PARENT, 1);

        message_count++;
    }

    // Close child's remaining pipe ends before exiting
    close(pipe_parent_to_child[0]);
    close(pipe_child_to_parent[1]);

    cleanup_resources(0);
    exit(0);
}

int main() {
    // Create pipes for bidirectional communication
    if (pipe(pipe_parent_to_child) == -1 || pipe(pipe_child_to_parent) == -1) {
        perror("pipe creation failed");
        exit(EXIT_FAILURE);
    }

    // Create two semaphores (using IPC_PRIVATE for a unique set)
    semaphore_id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0666);
    if (semaphore_id == -1) {
        perror("semaphore set creation failed");
        // Clean up pipes on error
        close(pipe_parent_to_child[0]);
        close(pipe_parent_to_child[1]);
        close(pipe_child_to_parent[0]);
        close(pipe_child_to_parent[1]);
        exit(EXIT_FAILURE);
    }

    // Initialize semaphores
    // SEMAPHORE_PARENT = 1 (parent goes first), SEMAPHORE_CHILD = 0 (child waits)
    if (semctl(semaphore_id, SEMAPHORE_PARENT, SETVAL, 1) == -1 || semctl(semaphore_id, SEMAPHORE_CHILD, SETVAL, 0) == -1) {
        perror("semaphore initialization failed");
        // Clean up pipes and semaphore set on error
        close(pipe_parent_to_child[0]);
        close(pipe_parent_to_child[1]);
        close(pipe_child_to_parent[0]);
        close(pipe_child_to_parent[1]);
        semctl(semaphore_id, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    // Register signal handler for SIGINT (Ctrl+C)
    if (signal(SIGINT, handle_termination_signal) == SIG_ERR) {
        perror("signal handler registration failed");
        // Clean up pipes and semaphore set on error
        close(pipe_parent_to_child[0]);
        close(pipe_parent_to_child[1]);
        close(pipe_child_to_parent[0]);
        close(pipe_child_to_parent[1]);
        semctl(semaphore_id, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    // Create child process
    child_process_id = fork();

    if (child_process_id == -1) {
        perror("fork failed");
        // Clean up pipes and semaphore set on error
        close(pipe_parent_to_child[0]);
        close(pipe_parent_to_child[1]);
        close(pipe_child_to_parent[0]);
        close(pipe_child_to_parent[1]);
        semctl(semaphore_id, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    if (child_process_id == 0) { // Child process
        child_process_logic();
    } else { // Parent process
        parent_process_logic();
    }

    return 0;
}