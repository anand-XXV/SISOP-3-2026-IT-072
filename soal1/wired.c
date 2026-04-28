#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"

// struct client
typedef struct {
    int  fd;
    char name[64];
    int  is_admin;
} Client;

// variabel global
Client  clients[MAX_CLIENTS];
int     client_count = 0;
time_t  server_start;
int     server_fd;

// fungsi logging ke history.log
void write_log(const char *who, const char *msg) {
    FILE *f = fopen("history.log", "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    fprintf(f, "[%s] [%s] [%s]\n", ts, who, msg);
    fclose(f);
}

// cek apakah nama sudah dipakai
int name_taken(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd > 0 && strcmp(clients[i].name, name) == 0)
            return 1;
    }
    return 0;
}

// broadcast ke semua client (kecuali pengirim)
void broadcast(const char *msg, int sender_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd > 0 && clients[i].fd != sender_fd && !clients[i].is_admin) {
            send(clients[i].fd, msg, strlen(msg), 0);
        }
    }
    write_log("User", msg);
}

// hapus client dari daftar
void remove_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            // broadcast notif disconnect ke semua (kecuali admin)
            if (!clients[i].is_admin) {
                char notif[BUF_SIZE];
                snprintf(notif, sizeof(notif), "[System] User '%s' has disconnected from The Wired.\n", clients[i].name);
                broadcast(notif, fd);
                write_log("System", notif + 9); // skip "[System] "

                char logmsg[BUF_SIZE];
                snprintf(logmsg, sizeof(logmsg), "User '%s' disconnected", clients[i].name);
                write_log("System", logmsg);
            }
            close(fd);
            clients[i].fd = 0;
            memset(clients[i].name, 0, sizeof(clients[i].name));
            clients[i].is_admin = 0;
            client_count--;
            return;
        }
    }
}

// hitung jumlah user aktif (bukan admin)
int count_active_users() {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd > 0 && !clients[i].is_admin)
            count++;
    }
    return count;
}

// handle signal (Ctrl+C) untuk shutdown bersih
void handle_shutdown(int sig) {
    // Broadcast ke semua client
    char msg[] = "[System] Server is shutting down. Goodbye.";
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd > 0)
            send(clients[i].fd, msg, strlen(msg), 0);
    }
    write_log("System", "EMERGENCY SHUTDOWN INITIATED");
    close(server_fd);
    exit(0);
}

// main
int main() {
    server_start = time(NULL);

    // setup signal handler
    signal(SIGINT, handle_shutdown);
    signal(SIGTERM, handle_shutdown);

    // inisialisasi array clients
    memset(clients, 0, sizeof(clients));

    // buat socket server
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    // supaya port bisa langsung dipakai ulang
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); exit(1);
    }

    printf("[Wired] Server online at port %d\n", SERVER_PORT);
    write_log("System", "SERVER ONLINE");

    fd_set master_set, read_set;
    FD_ZERO(&master_set);
    FD_SET(server_fd, &master_set);
    int max_fd = server_fd;

    // loop utama, pakai select() untuk multi-client
    while (1) {
        read_set = master_set;
        int activity = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (activity < 0) continue;

        // koneksi baru masuk
        if (FD_ISSET(server_fd, &read_set)) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
            if (new_fd < 0) { perror("accept"); continue; }

            // tambah ke slot kosong
            int added = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd == 0) {
                    clients[i].fd = new_fd;
                    clients[i].is_admin = 0;
                    FD_SET(new_fd, &master_set);
                    if (new_fd > max_fd) max_fd = new_fd;
                    client_count++;
                    added = 1;

                    // minta nama
                    send(new_fd, "Enter your name: ", 17, 0);
                    break;
                }
            }
            if (!added) {
                char *full = "[System] Server is full.\n";
                send(new_fd, full, strlen(full), 0);
                close(new_fd);
            }
        }

        // pesan dari client yang sudah terhubung
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = clients[i].fd;
            if (fd <= 0 || !FD_ISSET(fd, &read_set)) continue;

            char buf[BUF_SIZE];
            memset(buf, 0, sizeof(buf));
            int nbytes = recv(fd, buf, sizeof(buf) - 1, 0);

            // client disconnect
            if (nbytes <= 0) {
                FD_CLR(fd, &master_set);
                remove_client(fd);
                continue;
            }

            // hapus newline
            buf[strcspn(buf, "\r\n")] = 0;

            // belum punya nama maka proses registrasi nama
            if (strlen(clients[i].name) == 0) {
                // Batasi panjang nama maksimal 63 karakter
                buf[63] = '\0';
                // Cek apakah ini admin (nama "The Knights")
                if (strcmp(buf, "The Knights") == 0) {
                    // Minta password
                    send(fd, "Enter Password: ", 16, 0);
                    strcpy(clients[i].name, "__pending_admin__");
                    continue;
                }

                if (name_taken(buf)) {
                    char warn[BUF_SIZE];
                    snprintf(warn, sizeof(warn),
                             "[System] The identity '%s' is already synchronized in The Wired.\nEnter your name: ", buf);
                    send(fd, warn, strlen(warn), 0);
                    continue;
                }

                // nama valid maka disimpan
                strcpy(clients[i].name, buf);

                char welcome[BUF_SIZE];
                snprintf(welcome, sizeof(welcome), "--- Welcome to The Wired, %s ---\n", buf);
                send(fd, welcome, strlen(welcome), 0);

                char logmsg[BUF_SIZE];
                snprintf(logmsg, sizeof(logmsg), "User '%s' connected", buf);
                write_log("System", logmsg);

                // broadcast ke user lain
                char notif[BUF_SIZE];
                snprintf(notif, sizeof(notif), "[System] User '%s' has joined The Wired.\n", buf);
                broadcast(notif, fd);
                continue;
            }

            // verifikasi password admin
            if (strcmp(clients[i].name, "__pending_admin__") == 0) {
                if (strcmp(buf, ADMIN_PASSWORD) == 0) {
                    clients[i].is_admin = 1;
                    strcpy(clients[i].name, "The Knights");

                    send(fd, "[System] Authentication Successful. Granted Admin privileges.\n", 62, 0);

                    char menu[] =
                        "\n=== THE KNIGHTS CONSOLE ===\n"
                        "1. Check Active Entites (Users)\n"
                        "2. Check Server Uptime\n"
                        "3. Execute Emergency Shutdown\n"
                        "4. Disconnect\n"
                        "Command >> ";
                    send(fd, menu, strlen(menu), 0);

                    write_log("System", "User 'The Knights' connected");
                } else {
                    send(fd, "[System] Wrong password. Disconnecting.\n", 40, 0);
                    FD_CLR(fd, &master_set);
                    remove_client(fd);
                }
                continue;
            }

            // console admin
            if (clients[i].is_admin) {
                if (strcmp(buf, "1") == 0) {
                    write_log("Admin", "RPC_GET_USERS");
                    char resp[BUF_SIZE];
                    snprintf(resp, sizeof(resp), "[RPC] Active users: %d\nCommand >> ", count_active_users());
                    send(fd, resp, strlen(resp), 0);
                } else if (strcmp(buf, "2") == 0) {
                    write_log("Admin", "RPC_GET_UPTIME");
                    time_t now = time(NULL);
                    long uptime = (long)(now - server_start);
                    char resp[BUF_SIZE];
                    snprintf(resp, sizeof(resp), "[RPC] Server uptime: %ld seconds\nCommand >> ", uptime);
                    send(fd, resp, strlen(resp), 0);
                } else if (strcmp(buf, "3") == 0) {
                    write_log("Admin", "RPC_SHUTDOWN");
                    write_log("System", "EMERGENCY SHUTDOWN INITIATED");
                    char resp[] = "[System] Emergency shutdown initiated.\n";
                    // Broadcast ke semua
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (clients[j].fd > 0)
                            send(clients[j].fd, resp, strlen(resp), 0);
                    }
                    sleep(1);
                    close(server_fd);
                    exit(0);
                } else if (strcmp(buf, "4") == 0 || strcmp(buf, "/exit") == 0) {
                    send(fd, "[System] Disconnecting from The Wired...\n", 41, 0);
                    FD_CLR(fd, &master_set);
                    remove_client(fd);
                } else {
                    char menu[] =
                        "\n=== THE KNIGHTS CONSOLE ===\n"
                        "1. Check Active Entites (Users)\n"
                        "2. Check Server Uptime\n"
                        "3. Execute Emergency Shutdown\n"
                        "4. Disconnect\n"
                        "Command >> ";
                    send(fd, menu, strlen(menu), 0);
                }
                continue;
            }

            // user biasa, bisa melakukan /exit atau chat
            if (strcmp(buf, "/exit") == 0) {
                FD_CLR(fd, &master_set);
                remove_client(fd);
                continue;
            }

            // broadcast pesan chat ke semua user lain
            char chat[BUF_SIZE];
            snprintf(chat, sizeof(chat), "[%s]: %s\n", clients[i].name, buf);
            broadcast(chat, fd);
        }
    }

    return 0;
}