#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"

int sock_fd;
int named = 0;
volatile int disconnecting = 0;
int pipe_fd[2]; // pipe untuk wake up select() dari thread lain

// fungsi disconnect
void do_disconnect() {
    // __sync_bool_compare_and_swap: hanya set ke 1 jika nilai sekarang 0
    // jika sudah 1 (sudah disconnect), langsung return — thread lain tidak bisa masuk
    if (!__sync_bool_compare_and_swap(&disconnecting, 0, 1)) return;

    printf("\n[System] Disconnecting from The Wired...\n");
    fflush(stdout);
    // tulis ke pipe agar select() di main thread langsung wake up
    write(pipe_fd[1], "x", 1);
    close(sock_fd);
}

// thread: Menerima pesan dari server
void *receive_thread(void *arg) {
    char buf[BUF_SIZE];
    while (1) {
        memset(buf, 0, sizeof(buf));
        int nbytes = recv(sock_fd, buf, sizeof(buf) - 1, 0);

        if (nbytes <= 0) {
            do_disconnect();
            return NULL;
        }

        if (strstr(buf, "Welcome to The Wired")) {
            named = 1;
        }

        printf("\r%s", buf);
        fflush(stdout);

        if (named && !disconnecting) {
            printf("> ");
            fflush(stdout);
        }
    }
    return NULL;
}

// handle Ctrl+C
void handle_sigint(int sig) {
    send(sock_fd, "/exit", 5, 0);
    sleep(1);
    do_disconnect();
}

// main, pakai select() untuk monitor stdin
int main() {
    signal(SIGINT, handle_sigint);

    // Buat pipe untuk wake up select()
    if (pipe(pipe_fd) < 0) { perror("pipe"); exit(1); }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        exit(1);
    }

    // buat receive thread
    pthread_t tid;
    pthread_create(&tid, NULL, receive_thread, NULL);
    pthread_detach(tid);

    char buf[BUF_SIZE];
    while (1) {
        if (disconnecting) break;

        if (named && !disconnecting) {
            printf("> ");
            fflush(stdout);
        }

        // pakai select() untuk monitor stdin dan pipe sekaligus
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(pipe_fd[0], &readfds);
        int maxfd = pipe_fd[0] > STDIN_FILENO ? pipe_fd[0] : STDIN_FILENO;

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) break;

        // ada data di pipe = sinyal disconnect, keluar
        if (FD_ISSET(pipe_fd[0], &readfds)) break;

        // ada input dari user di stdin
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            memset(buf, 0, sizeof(buf));
            if (fgets(buf, sizeof(buf), stdin) == NULL) break;

            buf[strcspn(buf, "\n")] = 0;

            if (strlen(buf) == 0) continue;
            if (disconnecting) break;

            send(sock_fd, buf, strlen(buf), 0);

            if (strcmp(buf, "/exit") == 0) {
                sleep(1);
                do_disconnect();
                break;
            }
        }
    }

    exit(0);
    return 0;
}