# SISOP-3-2026-IT-072
# I Made Gyanendra Anand Wisnawa // 5027251072

## Soal 1

### Deskripsi Soal 1

Soal ini meminta implementasi sistem komunikasi berbasis **TCP socket** di C. Perintah yang ada di dalam soal yang perlu diimplementasikan yaitu:

1. **Koneksi stabil multi-client** – NAVI (client) terdaftar ke jaringan melalui IP dan port dari `protocol.h` tanpa mengganggu client lain yang sudah terhubung.
2. **Asynchronous client** – NAVI menjalankan dua fungsi sekaligus (kirim & terima pesan) **tanpa fork**, menggunakan thread.
3. **Scalable server** – Server menangani banyak client sekaligus menggunakan `select()`, membedakan koneksi baru vs pesan masuk, dan menangani disconnect via `/exit` maupun interrupt signal.
4. **Identitas unik** – Setiap client wajib memiliki nama unik; tidak boleh ada dua client dengan nama yang sama.
5. **Broadcast** – Setiap pesan dari satu client diteruskan ke semua client lain yang aktif.
6. **Admin RPC (The Knights)** – Akses eksklusif dengan autentikasi password untuk: cek jumlah user aktif, cek uptime server, dan emergency shutdown — tanpa melalui jalur broadcast.
7. **Logging** – Setiap pesan yang dibroadcast dicatat ke `history.log` dengan format `[YYYY-MM-DD HH:MM:SS] [System/Admin/User] [Status/Command/Chat]`.

### Proses Pengerjaan

#### `protocol.h`

File header berisi konstanta yang dipakai bersama oleh server dan client:

```
#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 8080
#define ADMIN_PASSWORD "admin123"
#define MAX_CLIENTS 32
#define BUF_SIZE 2048
```

#### `wired.c` (Server)

Server menggunakan `select()` untuk menangani banyak client sekaligus tanpa multithreading. Beberapa fungsi utama:

**`write_log()`** – Mencatat aktivitas ke `history.log` dengan format timestamp `[YYYY-MM-DD HH:MM:SS] [who] [msg]`.

```
void write_log(const char *who, const char *msg) {
    FILE *f = fopen("history.log", "a");
    // ...
    fprintf(f, "[%s] [%s] [%s]\n", ts, who, msg);
    fclose(f);
}
```

**`broadcast()`** – Mengirim pesan ke semua client yang aktif (selain pengirim dan admin), lalu mencatat ke log.

**Registrasi nama & autentikasi admin** – Saat client pertama kali connect, server meminta nama. Jika nama yang dimasukkan adalah `"The Knights"`, server meminta password admin. Jika cocok dengan `ADMIN_PASSWORD`, client mendapat hak admin dan bisa mengakses console khusus.

Fitur console admin:
- Pilihan `1` : Melihat jumlah user aktif (`RPC_GET_USERS`)
- Pilihan `2` : Melihat uptime server (`RPC_GET_UPTIME`)
- Pilihan `3` : Emergency shutdown (broadcast ke semua lalu `exit`)
- Pilihan `4` : Disconnect admin

**`handle_shutdown()`** – Signal handler untuk `SIGINT`/`SIGTERM`. Broadcast pesan shutdown ke semua client sebelum menutup server.

**Loop utama** menggunakan `select()` dengan `fd_set` untuk memonitor semua socket aktif sekaligus. Koneksi baru ditambahkan ke slot kosong di array `clients[MAX_CLIENTS]`.

#### `navi.c` (Client)

Client menggunakan **thread terpisah** untuk menerima pesan dari server sambil main thread menangani input user.

**`receive_thread()`** – Thread yang terus-menerus melakukan `recv()` dari server. Jika server mengirim pesan `"Welcome to The Wired"`, flag `named` di-set menjadi 1 (artinya registrasi nama berhasil).

**`do_disconnect()`** – Fungsi disconnect yang aman dari race condition menggunakan `__sync_bool_compare_and_swap`. Menulis ke pipe untuk membangunkan `select()` di main thread.

**Loop utama** memakai `select()` untuk memonitor dua file descriptor sekaligus: `STDIN_FILENO` (input user) dan `pipe_fd[0]` (sinyal disconnect dari thread lain).

```
FD_SET(STDIN_FILENO, &readfds);
FD_SET(pipe_fd[0], &readfds);
select(maxfd + 1, &readfds, NULL, NULL, NULL);
```

**`handle_sigint()`** – Mengirim perintah `/exit` ke server saat user menekan `Ctrl+C` sebelum disconnect.

### Edge Case & Error Handling

#### Server (`wired.c`)
 
**1. Server penuh (MAX_CLIENTS = 32)**
Jika semua slot di array `clients[MAX_CLIENTS]` sudah terisi, client baru yang mencoba connect langsung mendapat pesan `[System] Server is full.` dan socket-nya langsung ditutup (`close(new_fd)`). Server tidak crash dan tetap melayani client yang sudah terhubung.
```
if (!added) {
    char *full = "[System] Server is full.\n";
    send(new_fd, full, strlen(full), 0);
    close(new_fd);
}
```
 
**2. Nama duplikat**
Sebelum menyimpan nama client, server memanggil `name_taken()` yang mengiterasi seluruh slot. Jika nama sudah dipakai, server mengirim peringatan dan meminta nama lagi — tanpa memutus koneksi.
```
if (name_taken(buf)) {
    char warn[BUF_SIZE];
    snprintf(warn, sizeof(warn),
        "[System] The identity '%s' is already synchronized in The Wired.\nEnter your name: ", buf);
    send(fd, warn, strlen(warn), 0);
    continue; // tidak disconnect, ulangi input nama
}
```
 
**3. Client disconnect tiba-tiba (tanpa `/exit`)**
Saat `recv()` mengembalikan nilai `<= 0` (koneksi putus dari sisi client, misal terminal ditutup paksa), server langsung memanggil `remove_client()` yang: (a) broadcast notifikasi disconnect ke semua user lain, (b) menutup fd, (c) mengosongkan slot, (d) mencatat ke `history.log`. FD juga dikeluarkan dari `master_set` agar tidak di-poll lagi.
```
if (nbytes <= 0) {
    FD_CLR(fd, &master_set);
    remove_client(fd);
    continue;
}
```
 
**4. Password admin salah**
Jika nama client adalah `"The Knights"` tapi password yang dimasukkan tidak cocok, server langsung memutus koneksi client tersebut. State sementara `"__pending_admin__"` digunakan sebagai penanda bahwa client sedang dalam proses autentikasi.
```
if (strcmp(buf, ADMIN_PASSWORD) != 0) {
    send(fd, "[System] Wrong password. Disconnecting.\n", 40, 0);
    FD_CLR(fd, &master_set);
    remove_client(fd);
}
```
 
**5. Port sudah dipakai (address reuse)**
Menggunakan `SO_REUSEADDR` agar server bisa langsung bind ulang ke port yang sama setelah restart, tanpa harus menunggu timeout `TIME_WAIT` dari OS.
```
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```
 
**6. Race condition admin shutdown vs user disconnect**
Saat admin menjalankan Emergency Shutdown (pilihan `3`), server melakukan `sleep(1)` sebelum `exit(0)` agar semua pesan shutdown sempat terkirim ke client. Signal handler `handle_shutdown()` yang dipasang untuk `SIGINT`/`SIGTERM` juga melakukan hal yang sama.
 
**7. Nama terlalu panjang**
Buffer nama dibatasi secara eksplisit:
```
buf[63] = '\0'; // potong di 63 karakter sebelum disimpan
```
Ini mencegah buffer overflow ke struct `Client.name[64]`.
 
**8. Logging gagal buka file**
Fungsi `write_log()` melakukan pengecekan sebelum menulis. Jika `history.log` tidak bisa dibuka (misalnya permission issue), fungsi langsung return tanpa crash.
```
FILE *f = fopen("history.log", "a");
if (!f) return; // tidak crash, hanya skip logging
```
 
#### Client (`navi.c`)
 
**9. Koneksi ke server gagal**
Jika `connect()` gagal (server belum jalan atau IP/port salah), client langsung keluar dengan pesan error dari `perror("connect")`. Tidak ada retry loop — sesuai desain soal.
 
**10. Race condition double disconnect** 
Masalah utama yang sempat jadi kendala: ketika user menekan `/exit` dan sekaligus receive thread mendeteksi koneksi putus, bisa terjadi dua kali pesan "Disconnecting". Ini diselesaikan dengan atomic compare-and-swap: 
```
void do_disconnect() {
    // hanya bisa masuk sekali: jika disconnecting sudah 1, langsung return
    if (!__sync_bool_compare_and_swap(&disconnecting, 0, 1)) return;
    // ...
}
```
Dengan ini, hanya satu thread yang bisa "menang" dan menjalankan proses disconnect.
 
**11. Wake up `select()` dari thread lain**
`select()` di main thread bersifat blocking. Ketika receive thread mendeteksi server putus, tidak bisa langsung menghentikan main thread. Solusinya: pipe `pipe_fd[2]` digunakan sebagai "sinyal". Receive thread menulis `"x"` ke `pipe_fd[1]`, sehingga `pipe_fd[0]` yang di-monitor oleh `select()` menjadi readable, dan main thread keluar dari blocking state. 
```
write(pipe_fd[1], "x", 1); // wake up select() di main thread
```
 
**12. Input kosong diabaikan**
Agar tidak mengirim pesan kosong ke server:
```
if (strlen(buf) == 0) continue;
```

### Full Code Setiap File Soal 1
#### protocol.h
```
#ifndef PROTOCOL_H
#define PROTOCOL_H

// alamat dan port server
#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 8080

// password admin
#define ADMIN_PASSWORD "admin123"

// batas maksimal client
#define MAX_CLIENTS 32

// ukuran buffer pesan
#define BUF_SIZE 2048

#endif
```

#### navi.c
```
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
```

#### wired.c
```
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
```

### Cara Menjalankan Soal 1

**Kompilasi kode:**
```
# Server
gcc -o wired wired.c

# Client
gcc -o navi navi.c -lpthread
```

**Cara menjalankan kode:**
```b
# Terminal 1: jalankan server
./wired

# Terminal 2, 3, dst: jalankan client
./navi
```

**Perintah saat chat:**
- Ketik pesan lalu Enter, pesan yang diketik akan di broadcast ke semua user lain
- `/exit` untuk disconnect dari server

**Login sebagai admin:**
1. Jalankan `./navi`
2. Saat diminta nama, ketik `The Knights`
3. Masukkan password: `admin123`

### Screenshot Soal 1
<img width="953" height="55" alt="image" src="https://github.com/user-attachments/assets/5c434ff0-3313-47d0-bee1-b1ee141e6cc3" />
<img width="931" height="240" alt="image" src="https://github.com/user-attachments/assets/c8ad88c7-b1ef-4955-911c-4a909de977a2" />
<img width="956" height="233" alt="image" src="https://github.com/user-attachments/assets/f84d4fb6-da64-49af-b93d-b93787808a89" />
<img width="944" height="540" alt="image" src="https://github.com/user-attachments/assets/2f165d61-8539-4853-9817-a8583e75ba3c" />


### Kendala Soal 1

Terdapat banyak kendala saat pengerjaan soal, yang paling banyak masalahnya yaitu saat chatnya. Sempat saat /exit atau ctrl + c terjadi double sending message "disconnecting" atau tidak terdeteksinya login admin dengan The Knights. Dan saat implementasi admin pun terdapat error yang sulit pada bagian force shutdown, dimana muncul message disconnecting tetapi tidak terdisconnect.

---

## Soal 2

### Deskripsi Soal 2

Soal ini meminta implementasi sistem **battle arena multiplayer** berbasis **IPC** di C. Perintah dari soal yang harus diimplementasikan di dalam kodenya yaitu:

1. **Setup IPC** – `arena.h` berisi semua definisi struct, config, dan IPC key. `orion.c` sebagai server, `eternal.c` sebagai client, komunikasi **hanya via IPC** (bukan RPC/socket).
2. **Main menu** – Tampilan awal eternal: Register, Login, Exit. Orion harus selalu siap menerima koneksi.
3. **Komunikasi via Message Queue & Shared Memory** – Jika orion belum berjalan, eternal langsung gagal dengan pesan `"Orion are you there?"`.
4. **Register & Login** – Hanya butuh username dan password. Username unik. Data **persistent** (tetap ada meski eternal mati). Tidak bisa login ganda di sesi berbeda.
5. **Properti default player** – Gold: 150, Lvl: 1, XP: 0.
6. **Matchmaking** – Berlangsung 35 detik; jika tidak ada lawan, melawan bot. Player yang sedang battle tidak terdeteksi di matchmaking.
7. **Battle realtime (asynchronous)** – Tombol `a` untuk Attack, `u` untuk Ultimate (hanya jika punya weapon). Cooldown attack 1 detik. Tampil 5 combat log teratas dan HP realtime. Base damage: 10, base health: 100.
8. **Reward setelah battle** – XP: +50 (menang) / +15 (kalah). Gold: +120 (menang) / +30 (kalah). Level naik tiap kelipatan 100 XP. Damage = `BASE_DMG + (total_xp/50) + weapon_bonus`. Health = `BASE_HEALTH + (total_xp/10)`.
9. **Armory** – Player bisa beli senjata. Senjata dengan damage terbesar otomatis aktif. Memiliki senjata mengaktifkan Ultimate. Formula Ultimate = `Total Damage * 3`.
10. **Match History** – Menyimpan riwayat pertandingan: waktu, lawan, hasil (WIN/LOSS), XP didapat.

**Additional Notes:** Gunakan Semaphore atau Mutex untuk mencegah race condition.

### Proses pengerjaan

#### `arena.h`

Header utama yang berisi semua definisi shared antara server dan client:

**Kunci IPC:**
```
#define SHM_KEY_PLAYERS  0x00001234  // shared memory untuk data pemain
#define SHM_KEY_ARENA    0x00005678  // shared memory untuk state arena/battle
#define SEM_KEY          0x00009012  // semaphore
#define MQ_KEY           0x00009999  // message queue
```

**Struct `Player`** – disimpan di shared memory, berisi username, password, gold, level, XP, weapon, status, PID, lawan, HP, dan match history.

**Struct `Message`** – untuk message queue, berisi `mtype` (PID client sebagai target) dan `mtext` (isi pesan/command).

**Konfigurasi battle:**
```
#define BASE_DAMAGE      10
#define BASE_HEALTH      100
#define ATTACK_COOLDOWN  1      // detik
#define MATCHMAKING_TIME 35     // detik
```

**Daftar senjata (`WEAPONS[5]`):**
- Wood Sword | 100 gold | +5 damage|
- Iron Sword | 300 gold | +15 damage|
- Steel Axe | 600 gold | +30 damage|
- Demon Blade | 1500 gold | +60 damage|
- God Slayer | 5000 gold | +150 damage|

#### `orion.c` (Server)

Server menginisialisasi semua resource IPC saat startup lalu masuk ke loop utama yang mendengarkan message queue.

**Inisialisasi IPC:**
```
shm_id = shmget(SHM_KEY_PLAYERS, sizeof(SharedPlayers), IPC_CREAT | 0666);
mq_id  = msgget(MQ_KEY, IPC_CREAT | 0666);
sem_id = semget(SEM_KEY, 1, IPC_CREAT | 0666);
```

**Format command dari client:** `"<PID>:<CMD>:<arg1>:<arg2>"`

**Daftar command yang ditangani server:**

- `REGISTER:<user>:<pass>`: Registrasi akun baru
- `LOGIN:<user>:<pass>`: Login, set status ONLINE, simpan PID
- `LOGOUT`: Logout, set status OFFLINE
- `MATCHMAKING`: Masuk antrian matchmaking, cari lawan atau bot
- `ATTACK`: Serang lawan saat battle
- `ULTIMATE`: Serangan ultimate (damage lebih besar)
- `BATTLE_END:<result>`: Akhiri battle, hitung reward XP & gold
- `BUY:<weapon_idx>`: Beli senjata dari armory
- `STATS`: Kirim data stats player ke client
- `HISTORY`: Kirim match history ke client
- `BSTAT`: Kirim battle status (HP pemain & lawan)

**Matchmaking:** Server mencari player lain yang statusnya `STATUS_MATCHMAKING`. Jika tidak ada dalam `MATCHMAKING_TIME` detik, player dihadapkan dengan **bot** dengan HP dan level acak.

**Signal handler** (`SIGINT`/`SIGTERM`) membersihkan semua resource IPC sebelum keluar:
```c
shmctl(shm_id, IPC_RMID, NULL);
msgctl(mq_id, IPC_RMID, NULL);
semctl(sem_id, 0, IPC_RMID);
```

#### `eternal.c` (Client)

Client menyediakan antarmuka TUI (Text User Interface) interaktif dengan beberapa menu:

**`menu_register()`** – Form registrasi akun. Mengirim command `REGISTER` ke server dan menunggu respons.

**`menu_main()`** – Menu utama setelah login, menampilkan stats terkini (level, XP, gold, senjata). Pilihan menu:
1. Matchmaking
2. Armory (beli senjata)
3. Match History
4. Logout

**`menu_matchmaking()`** – Memasukkan player ke antrian, menunggu lawan ditemukan, lalu memanggil `do_battle()`.

**`do_battle()`** – Layar battle utama dengan:
- Menampilkan HP kedua pemain secara real-time (`draw_battle()`)
- Input: `a` untuk Attack, `u` untuk Ultimate, `q` untuk menyerah
- Cooldown attack dan ultimate ditampilkan di layar
- **`battle_recv_thread()`** berjalan paralel untuk menerima update dari server

**`menu_armory()`** – Menampilkan daftar senjata beserta harga dan bonus damage. Player bisa membeli senjata jika gold mencukupi.

**`menu_history()`** – Menampilkan riwayat pertandingan: waktu, lawan, hasil (WIN/LOSS), dan XP yang didapat.

**Komunikasi client → server:**
```
void send_to_server(const char *cmd) {
    Message msg;
    msg.mtype = 1;  // server selalu listen di mtype=1
    snprintf(msg.mtext, sizeof(msg.mtext), "%ld:%s", my_pid, cmd);
    msgsnd(mq_id, &msg, sizeof(msg.mtext), 0);
}
```

**Komunikasi server → client** menggunakan PID client sebagai `mtype`, sehingga setiap client hanya menerima pesan yang ditujukan untuknya.

### Edge Case & Error Handling
 
#### Client (`eternal.c`)
 
**1. Orion belum berjalan**
Saat `eternal` dijalankan, hal pertama yang dilakukan adalah mencoba terhubung ke message queue server via `msgget(MQ_KEY, 0666)`. Jika server belum jalan (IPC belum dibuat), `msgget` akan gagal dan client langsung keluar dengan pesan yang jelas.
```
mq_id = msgget(MQ_KEY, 0666);
if (mq_id < 0) {
    printf("  Orion are you there?\n");
    exit(1);
}
```
Perhatikan bahwa flag yang digunakan adalah `0666` tanpa `IPC_CREAT` — artinya client hanya bisa join ke IPC yang sudah ada, bukan membuat sendiri.
 
**2. Respons error dari server ditampilkan ke user**
Semua respons yang diawali `"ERR:"` ditangani secara seragam di sisi client. Error message dari server langsung ditampilkan ke terminal.
```
} else if (strncmp(buf, "ERR:", 4) == 0) {
    printf("  %s\n", buf + 4);
}
```
Contoh: jika username duplikat saat register → `ERR:Username already exists.` → ditampilkan sebagai `Username already exists.`
 
**3. Pembelian senjata dengan weapon index tidak valid**
Saat user memasukkan pilihan armory di luar range 1-5:
```
if (choice < 1 || choice > MAX_WEAPONS) {
    printf("  Pilihan tidak valid.\n");
}
```
Validasi ini ada di sisi client sebagai defense pertama, sebelum command dikirim ke server.
 
**4. Terminal mode restore setelah battle**
Saat battle, terminal diubah ke raw mode (tanpa buffering, tanpa echo) agar input `a`/`u`/`q` bisa ditangkap langsung. Jika battle selesai atau program diinterrupt, `restore_mode()` dipanggil untuk mengembalikan terminal ke kondisi normal — mencegah terminal "rusak" setelah program selesai.
 
**5. HP tidak bisa di bawah nol**
Baik saat battle vs player maupun vs bot, HP diklem ke minimum 0:
```
if (bot_hp < 0) bot_hp = 0;
// dan
if (my_hp < 0) my_hp = 0;
```
 
**6. SIGINT (Ctrl+C) saat battle atau di menu**
Signal handler `handle_exit()` dipasang di awal `main()`. Ketika Ctrl+C ditekan, handler akan: (a) kirim command `LOGOUT` ke server, (b) restore terminal mode, (c) keluar dengan bersih — memastikan status player di shared memory kembali ke OFFLINE.
 
#### Server (`orion.c`)
 
**7. Username duplikat saat register**
Sebelum membuat slot player baru, server mengecek seluruh shared memory via `find_player()`. Jika username sudah ada, langsung kirim error tanpa mengubah data.
```
if (find_player(username) != -1) {
    sem_unlock();
    send_msg(pid, "ERR:Username already exists.");
    return;
}
```
 
**8. Shared memory penuh**
Jika `find_empty_slot()` tidak menemukan slot kosong (semua 32 slot terpakai), server menolak registrasi.
```
int slot = find_empty_slot();
if (slot == -1) {
    sem_unlock();
    send_msg(pid, "ERR:Server penuh.");
    return;
}
```
 
**9. Login ganda pada satu akun**
Server mengecek `status != STATUS_OFFLINE` sebelum mengizinkan login. Jika akun sedang dipakai (ONLINE, MATCHMAKING, atau BATTLE), login dari session lain ditolak.
```
if (p->status != STATUS_OFFLINE) {
    sem_unlock();
    send_msg(pid, "ERR:Account is already in use.");
    return;
}
```
 
**10. Password salah saat login**
Server membandingkan password yang disimpan di shared memory dengan yang dikirim client. Jika tidak cocok, dikirim error tanpa mengubah status player.
```
if (strcmp(p->password, password) != 0) {
    sem_unlock();
    send_msg(pid, "ERR:Wrong password.");
    return;
}
```
 
**11. Matchmaking tidak menemukan lawan → lawan bot**
Setelah `MATCHMAKING_TIME` (35 detik) tanpa menemukan player lain yang juga matchmaking, server mengatur status player ke `STATUS_BATTLE` dengan `opponent_idx = -1` (penanda bot) dan mengirim `MATCH:BOT` ke client.
```
shm->players[my_idx].status = STATUS_BATTLE;
shm->players[my_idx].opponent_idx = -1; // -1 = lawan bot
send_msg(pid, "MATCH:BOT");
```
 
**12. Player yang sedang battle tidak bisa dijadikan lawan matchmaking**
Dalam loop matchmaking, server hanya mempertimbangkan player dengan status `STATUS_MATCHMAKING`. Player dengan status `STATUS_BATTLE` atau `STATUS_ONLINE` otomatis tidak terdeteksi.
```
if (shm->players[i].status == STATUS_MATCHMAKING) {
    // pasangkan sebagai lawan
}
```
 
**13. Race condition matchmaking dengan semaphore**
Skenario masalah: dua client bisa saja saling menemukan satu sama lain secara bersamaan (thread A menemukan thread B, thread B menemukan thread A), lalu keduanya coba memasangkan dirinya. Ini dicegah dengan semaphore yang dikunci (`sem_lock()`) selama proses pencocokan. Setelah dipasangkan oleh satu thread, thread lain akan melihat status sudah `STATUS_BATTLE` dan berhenti.
```
sem_lock();
if (shm->players[my_idx].status == STATUS_BATTLE) {
    sem_unlock();
    return NULL; // sudah dipasangkan oleh thread lain
}
sem_unlock();
```
 
**14. Gold tidak cukup beli senjata**
Sebelum mengurangi gold, server memvalidasi saldo:
```
if (p->gold < w->price) {
    sem_unlock();
    send_msg(pid, "ERR:Gold tidak cukup.");
    return;
}
```
 
**15. Senjata otomatis menggunakan bonus damage terbesar**
Jika player membeli senjata yang bonus damage-nya lebih kecil dari yang sudah dimiliki, server tidak mengganti weapon aktif:
```
if (w->bonus_dmg > p->weapon_bonus_dmg) {
    p->weapon_bonus_dmg = w->bonus_dmg;
    strncpy(p->weapon_name, w->name, 31);
}
```
 
**16. Match history terbatas 50 entri**
Jika `history_count` sudah mencapai `MAX_HISTORY (50)`, entri baru tidak disimpan. Ini mencegah overflow di struct `Player`.
```
if (me->history_count < MAX_HISTORY) {
    // simpan history
    me->history_count++;
}
```
 
**17. Cleanup IPC saat server dimatikan**
Signal handler `cleanup()` untuk `SIGINT`/`SIGTERM` memastikan semua resource IPC dibersihkan sebelum proses berakhir, sehingga tidak ada "IPC zombie" yang tertinggal di sistem.
```
void cleanup(int sig) {
    shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);
    msgctl(mq_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    exit(0);
}
```
Jika server crash tanpa sempat cleanup, gunakan `make clear_ipc` untuk membersihkan resource IPC yang tersisa secara manual.
 
**18. Semua akses shared memory dilindungi semaphore**
Setiap fungsi handler (`handle_register`, `handle_login`, `handle_attack`, dll.) selalu memanggil `sem_lock()` di awal dan `sem_unlock()` sebelum return — termasuk di semua jalur error — untuk memastikan semaphore tidak pernah tertinggal dalam kondisi terkunci (deadlock).

### Full Code Setiap File Soal 2
#### Makefile
```
CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -lrt

all: server client

server: orion.c arena.h
	$(CC) $(CFLAGS) orion.c -o orion $(LDFLAGS)

client: eternal.c arena.h
	$(CC) $(CFLAGS) eternal.c -o eternal $(LDFLAGS)

clean:
	rm -f orion eternal

clear_ipc:
	ipcs -m | grep 0x00001234 | awk '{print $$2}' | xargs -r ipcrm -m
	ipcs -q | grep 0x00005678 | awk '{print $$2}' | xargs -r ipcrm -q
	ipcs -s | grep 0x00009012 | awk '{print $$2}' | xargs -r ipcrm -s
```

#### arena.h
```
#ifndef ARENA_H
#define ARENA_H

#include <sys/types.h>

// konfigurasi umum
#define MAX_PLAYERS     32
#define MAX_USERNAME    32
#define MAX_PASSWORD    32
#define MAX_HISTORY     50
#define NAME_LEN        32

// kunci IPC
#define SHM_KEY_PLAYERS  0x00001234  // shared memory untuk data pemain
#define SHM_KEY_ARENA    0x00005678  // shared memory untuk state arena/battle
#define SEM_KEY          0x00009012  // semaphore untuk mutual exclusion
#define MQ_KEY           0x00009999  // message queue untuk komunikasi

// konfigurasi battle
#define BASE_DAMAGE      10
#define BASE_HEALTH      100
#define ATTACK_COOLDOWN  1          // detik
#define MATCHMAKING_TIME 35         // detik

// konfigurasi default untuk player
#define DEFAULT_GOLD     150
#define DEFAULT_LVL      1
#define DEFAULT_XP       0

// XP dan gold reward
#define XP_WIN           50
#define XP_LOSE          15
#define GOLD_WIN         120
#define GOLD_LOSE        30

// status player
#define STATUS_OFFLINE      0
#define STATUS_ONLINE       1
#define STATUS_MATCHMAKING  2
#define STATUS_BATTLE       3

// list weapons
#define MAX_WEAPONS 5

typedef struct {
    char name[32];
    int  price;
    int  bonus_dmg;
} Weapon;

static const Weapon WEAPONS[MAX_WEAPONS] = {
    {"Wood Sword",  100,   5},
    {"Iron Sword",  300,  15},
    {"Steel Axe",   600,  30},
    {"Demon Blade", 1500, 60},
    {"God Slayer",  5000, 150}
};

// struct match history
typedef struct {
    char time[16];       // format HH:MM
    char opponent[NAME_LEN];
    int  result;         // 1 = WIN, 0 = LOSS
    int  xp_gained;
} MatchHistory;

// struct player (disimpan di shared memory)
typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    int  gold;
    int  lvl;
    int  xp;
    int  weapon_bonus_dmg;   // total bonus damage dari senjata
    char weapon_name[32];    // nama senjata aktif
    int  status;             // STATUS_OFFLINE/ONLINE/MATCHMAKING/BATTLE
    int  pid;                // PID process eternal yang login
    int  opponent_idx;       // index lawan di array players (-1 jika tidak ada)
    int  health;             // HP saat battle
    int  is_registered;      // 1 jika slot terpakai
    MatchHistory history[MAX_HISTORY];
    int  history_count;
} Player;

// struct shared memory players
typedef struct {
    Player players[MAX_PLAYERS];
    int    player_count;
} SharedPlayers;

// struct message queue
typedef struct {
    long mtype;              // tipe pesan (PID client sebagai target)
    char mtext[512];         // isi pesan
} Message;

#endif
```

#### orion.c
```
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <pthread.h>
#include "arena.h"

// struct untuk passing argument ke thread matchmaking
typedef struct {
    long pid;
} MatchArg;

// variabel global ipc
int shm_id;
int mq_id;
int sem_id;
SharedPlayers *shm;

// semaphore helper
void sem_lock() {
    struct sembuf op = {0, -1, 0};
    semop(sem_id, &op, 1);
}

void sem_unlock() {
    struct sembuf op = {0, 1, 0};
    semop(sem_id, &op, 1);
}

// kirim pesan ke client (via message queue)
void send_msg(long pid, const char *text) {
    Message msg;
    msg.mtype = pid;
    strncpy(msg.mtext, text, sizeof(msg.mtext) - 1);
    msg.mtext[sizeof(msg.mtext) - 1] = '\0';
    msgsnd(mq_id, &msg, sizeof(msg.mtext), 0);
}

// cari player berdasarkan username
int find_player(const char *username) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            strcmp(shm->players[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

// cari slot kosong
int find_empty_slot() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!shm->players[i].is_registered) return i;
    }
    return -1;
}

// kalkulasi dmg dan hp
int calc_damage(Player *p) {
    return BASE_DAMAGE + (p->xp / 50) + p->weapon_bonus_dmg;
}

int calc_health(Player *p) {
    return BASE_HEALTH + (p->xp / 10);
}

// handle register
void handle_register(long pid, const char *username, const char *password) {
    sem_lock();

    // cek apakah username sudah ada
    if (find_player(username) != -1) {
        sem_unlock();
        send_msg(pid, "ERR:Username already exists.");
        return;
    }

    // cari slot kosong
    int slot = find_empty_slot();
    if (slot == -1) {
        sem_unlock();
        send_msg(pid, "ERR:Server penuh.");
        return;
    }

    // isi data player baru
    Player *p = &shm->players[slot];
    memset(p, 0, sizeof(Player));
    strncpy(p->username, username, MAX_USERNAME - 1);
    strncpy(p->password, password, MAX_PASSWORD - 1);
    p->gold           = DEFAULT_GOLD;
    p->lvl            = DEFAULT_LVL;
    p->xp             = DEFAULT_XP;
    p->weapon_bonus_dmg = 0;
    strcpy(p->weapon_name, "None");
    p->status         = STATUS_OFFLINE;
    p->is_registered  = 1;
    p->opponent_idx   = -1;
    p->history_count  = 0;
    shm->player_count++;

    sem_unlock();
    send_msg(pid, "OK:Account created!");
}

// handle login
void handle_login(long pid, const char *username, const char *password) {
    sem_lock();

    int idx = find_player(username);
    if (idx == -1) {
        sem_unlock();
        send_msg(pid, "ERR:Username not found.");
        return;
    }

    Player *p = &shm->players[idx];

    if (strcmp(p->password, password) != 0) {
        sem_unlock();
        send_msg(pid, "ERR:Wrong password.");
        return;
    }

    if (p->status != STATUS_OFFLINE) {
        sem_unlock();
        send_msg(pid, "ERR:Account is already in use.");
        return;
    }

    p->status = STATUS_ONLINE;
    p->pid    = (int)pid;

    sem_unlock();

    // kirim data player ke client
    char resp[512];
    snprintf(resp, sizeof(resp), "OK:%d:%d:%d:%d:%s",
             p->gold, p->lvl, p->xp, p->weapon_bonus_dmg, p->weapon_name);
    send_msg(pid, resp);
}

// handle logout
void handle_logout(long pid) {
    sem_lock();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            shm->players[i].status = STATUS_OFFLINE;
            shm->players[i].pid    = 0;
            break;
        }
    }
    sem_unlock();
}

// handle matchmaking
void *matchmaking_thread(void *arg) {
    long pid = ((MatchArg *)arg)->pid;
    free(arg);
    sem_lock();

    // cari index player yang request matchmaking
    int my_idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            my_idx = i;
            break;
        }
    }

    if (my_idx == -1) {
        sem_unlock();
        send_msg(pid, "ERR:Player not found.");
        return NULL;
    }

    shm->players[my_idx].status = STATUS_MATCHMAKING;
    shm->players[my_idx].opponent_idx = -1;

    sem_unlock();

    // tunggu maksimal MATCHMAKING_TIME detik mencari lawan

    for (int t = 0; t < MATCHMAKING_TIME; t++) {
        sleep(1);

        sem_lock();
        // cari player lain yang juga matchmaking (bukan diri sendiri, bukan sedang battle)
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == my_idx) continue;
            if (shm->players[i].is_registered &&
                shm->players[i].status == STATUS_MATCHMAKING) {
                // jika cocok, pasangkan keduanya
                shm->players[my_idx].opponent_idx = i;
                shm->players[i].opponent_idx      = my_idx;
                shm->players[my_idx].status       = STATUS_BATTLE;
                shm->players[i].status            = STATUS_BATTLE;

                // set HP awal
                shm->players[my_idx].health = calc_health(&shm->players[my_idx]);
                shm->players[i].health      = calc_health(&shm->players[i]);


                sem_unlock();

                // beritahu kedua player
                char msg1[512], msg2[512];
                snprintf(msg1, sizeof(msg1), "MATCH:%s", shm->players[i].username);
                snprintf(msg2, sizeof(msg2), "MATCH:%s", shm->players[my_idx].username);
                send_msg(pid, msg1);
                send_msg(shm->players[i].pid, msg2);
                return NULL;
            }
        }
        sem_unlock();

        // CEK: jika sudah dipasangkan oleh thread lain, berhenti
        sem_lock();
        if (shm->players[my_idx].status == STATUS_BATTLE) {
            sem_unlock();
            return NULL;
        }
        sem_unlock();

        // beritahu client posisi timer
        char tick[64];
        snprintf(tick, sizeof(tick), "WAIT:%d", t + 1);
        send_msg(pid, tick);
    }

    // tidak ketemu lawan, lawan bot
    sem_lock();
    shm->players[my_idx].status       = STATUS_BATTLE;
    shm->players[my_idx].opponent_idx = -1;
    shm->players[my_idx].health       = calc_health(&shm->players[my_idx]);
    sem_unlock();

    send_msg(pid, "MATCH:BOT");
    return NULL;
}

// handle attack
void handle_attack(long pid, int is_ultimate) {
    sem_lock();

    int my_idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            my_idx = i;
            break;
        }
    }

    if (my_idx == -1) { sem_unlock(); return; }

    Player *me  = &shm->players[my_idx];
    int opp_idx = me->opponent_idx;

    int dmg = calc_damage(me);
    if (is_ultimate) dmg *= 3;

    char result[512];

    if (opp_idx == -1) {
        // lawan bot, bot HP = 100, kurangi langsung
        snprintf(result, sizeof(result), "ATK:%d:BOT", dmg);
        sem_unlock();
        send_msg(pid, result);
        return;
    }

    Player *opp = &shm->players[opp_idx];
    opp->health -= dmg;
    if (opp->health < 0) opp->health = 0;

    snprintf(result, sizeof(result), "ATK:%d:%s:%d",
             dmg, opp->username, opp->health);
    sem_unlock();

    // beritahu penyerang
    send_msg(pid, result);

    // beritahu yang diserang
    char notif[512];
    snprintf(notif, sizeof(notif), "HIT:%d:%s:%d",
             dmg, me->username, opp->health);
    send_msg(opp->pid, notif);
}

// handle battle end
void handle_battle_end(long pid, int win) {
    sem_lock();

    int my_idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            my_idx = i;
            break;
        }
    }

    if (my_idx == -1) { sem_unlock(); return; }

    Player *me = &shm->players[my_idx];

    // update XP dan Gold
    int xp_gain   = win ? XP_WIN   : XP_LOSE;
    int gold_gain = win ? GOLD_WIN : GOLD_LOSE;
    me->xp   += xp_gain;
    me->gold += gold_gain;

    // update level (naik setiap kelipatan 100 XP)
    me->lvl = 1 + (me->xp / 100);

    // simpan history
    if (me->history_count < MAX_HISTORY) {
        MatchHistory *h = &me->history[me->history_count];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        snprintf(h->time, sizeof(h->time), "%02d:%02d", t->tm_hour, t->tm_min);

        int opp_idx = me->opponent_idx;
        if (opp_idx >= 0)
            strncpy(h->opponent, shm->players[opp_idx].username, NAME_LEN - 1);
        else
            strcpy(h->opponent, "BOT");

        h->result    = win;
        h->xp_gained = xp_gain;
        me->history_count++;
    }

    // reset status
    me->status       = STATUS_ONLINE;
    me->opponent_idx = -1;
    me->health       = 0;

    sem_unlock();

    // kirim update stats ke client
    char resp[512];
    snprintf(resp, sizeof(resp), "END:%d:%d:%d:%d",
             win, xp_gain, gold_gain, me->lvl);
    send_msg(pid, resp);
}

// handle get stats (untuk refresh tampilan)
void handle_get_stats(long pid) {
    sem_lock();
    int idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            idx = i;
            break;
        }
    }
    if (idx == -1) { sem_unlock(); return; }

    Player *p = &shm->players[idx];
    char resp[512];
    snprintf(resp, sizeof(resp), "STATS:%d:%d:%d:%d:%s",
             p->gold, p->lvl, p->xp, p->weapon_bonus_dmg, p->weapon_name);
    sem_unlock();
    send_msg(pid, resp);
}

// handle buy weapon
void handle_buy_weapon(long pid, int weapon_idx) {
    sem_lock();

    int idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            idx = i;
            break;
        }
    }

    if (idx == -1) { sem_unlock(); return; }
    if (weapon_idx < 0 || weapon_idx >= MAX_WEAPONS) {
        sem_unlock();
        send_msg(pid, "ERR:Senjata tidak valid.");
        return;
    }

    Player *p = &shm->players[idx];
    const Weapon *w = &WEAPONS[weapon_idx];

    if (p->gold < w->price) {
        sem_unlock();
        send_msg(pid, "ERR:Gold tidak cukup.");
        return;
    }

    p->gold -= w->price;

    // pakai senjata dengan bonus damage terbesar
    if (w->bonus_dmg > p->weapon_bonus_dmg) {
        p->weapon_bonus_dmg = w->bonus_dmg;
        strncpy(p->weapon_name, w->name, 31);
    }

    char resp[512];
    snprintf(resp, sizeof(resp), "OK:Berhasil membeli %s! Gold tersisa: %d",
             w->name, p->gold);
    sem_unlock();
    send_msg(pid, resp);
}

// handle get history
void handle_get_history(long pid) {
    sem_lock();
    int idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            idx = i;
            break;
        }
    }
    if (idx == -1) { sem_unlock(); return; }

    Player *p = &shm->players[idx];
    char resp[512];

    if (p->history_count == 0) {
        sem_unlock();
        send_msg(pid, "HIST:EMPTY");
        return;
    }

    // kirim history satu per satu
    for (int i = p->history_count - 1; i >= 0; i--) {
        MatchHistory *h = &p->history[i];
        snprintf(resp, sizeof(resp), "HIST:%s:%s:%s:+%dXP",
                 h->time,
                 h->opponent,
                 h->result ? "WIN" : "LOSS",
                 h->xp_gained);
        send_msg(pid, resp);
    }
    sem_unlock();
    send_msg(pid, "HIST:DONE");
}

// handle get opponent hp (untuk battle display)
void handle_get_battle_status(long pid) {
    sem_lock();
    int idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shm->players[i].is_registered &&
            shm->players[i].pid == (int)pid) {
            idx = i;
            break;
        }
    }
    if (idx == -1) { sem_unlock(); return; }

    Player *me = &shm->players[idx];
    int opp_idx = me->opponent_idx;

    char resp[512];
    if (opp_idx >= 0) {
        Player *opp = &shm->players[opp_idx];
        snprintf(resp, sizeof(resp), "BSTAT:%d:%d:%s:%d:%d:%d",
                 me->health,
                 calc_health(me),
                 opp->username,
                 opp->health,
                 calc_health(opp),
                 opp->lvl);
    } else {
        snprintf(resp, sizeof(resp), "BSTAT:%d:%d:BOT:100:100:1",
                 me->health, calc_health(me));
    }
    sem_unlock();
    send_msg(pid, resp);
}

// cleanup ipc saat signal
void cleanup(int sig) {
    printf("\n[Orion] Shutting down...\n");
    shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);
    msgctl(mq_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    exit(0);
}

// main
int main() {
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // buat Shared Memory
    shm_id = shmget(SHM_KEY_PLAYERS, sizeof(SharedPlayers), IPC_CREAT | 0666);
    if (shm_id < 0) { perror("shmget"); exit(1); }

    shm = (SharedPlayers *)shmat(shm_id, NULL, 0);
    if (shm == (void *)-1) { perror("shmat"); exit(1); }

    // inisialisasi shared memory
    memset(shm, 0, sizeof(SharedPlayers));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        shm->players[i].opponent_idx = -1;
    }

    // buat Message Queue
    mq_id = msgget(MQ_KEY, IPC_CREAT | 0666);
    if (mq_id < 0) { perror("msgget"); exit(1); }

    // buat Semaphore
    sem_id = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (sem_id < 0) { perror("semget"); exit(1); }
    semctl(sem_id, 0, SETVAL, 1); // inisialisasi semaphore = 1

    printf("[Orion] Orion is ready (PID: %d)\n", getpid());

    // loop utama, terima dan proses pesan dari client
    Message msg;
    while (1) {
        // terima pesan dari client manapun (mtype = 1 untuk request ke server)
        if (msgrcv(mq_id, &msg, sizeof(msg.mtext), 1, 0) < 0) continue;

        char cmd[32];
        char arg1[MAX_USERNAME];
        char arg2[MAX_PASSWORD];
        long client_pid;
        int  iarg1;

        // parse pesan: FORMAT = "PID:COMMAND:arg1:arg2"
        // contoh: "12345:REGISTER:john:pass123"
        char buf[512];
        strncpy(buf, msg.mtext, sizeof(buf) - 1);

        char *token = strtok(buf, ":");
        if (!token) continue;
        client_pid = atol(token);

        token = strtok(NULL, ":");
        if (!token) continue;
        strncpy(cmd, token, sizeof(cmd) - 1);

        // dispatch command
        if (strcmp(cmd, "REGISTER") == 0) {
            token = strtok(NULL, ":"); if (!token) continue;
            strncpy(arg1, token, MAX_USERNAME - 1);
            token = strtok(NULL, ":"); if (!token) continue;
            strncpy(arg2, token, MAX_PASSWORD - 1);
            handle_register(client_pid, arg1, arg2);

        } else if (strcmp(cmd, "LOGIN") == 0) {
            token = strtok(NULL, ":"); if (!token) continue;
            strncpy(arg1, token, MAX_USERNAME - 1);
            token = strtok(NULL, ":"); if (!token) continue;
            strncpy(arg2, token, MAX_PASSWORD - 1);
            handle_login(client_pid, arg1, arg2);

        } else if (strcmp(cmd, "LOGOUT") == 0) {
            handle_logout(client_pid);

        } else if (strcmp(cmd, "MATCHMAKING") == 0) {
            // jalankan matchmaking di thread terpisah agar server tidak blocking
            MatchArg *marg = malloc(sizeof(MatchArg));
            marg->pid = client_pid;
            pthread_t mm_tid;
            pthread_create(&mm_tid, NULL, matchmaking_thread, marg);
            pthread_detach(mm_tid);

        } else if (strcmp(cmd, "ATTACK") == 0) {
            handle_attack(client_pid, 0);

        } else if (strcmp(cmd, "ULTIMATE") == 0) {
            handle_attack(client_pid, 1);

        } else if (strcmp(cmd, "BATTLE_END") == 0) {
            token = strtok(NULL, ":"); if (!token) continue;
            iarg1 = atoi(token); // 1 = win, 0 = lose
            handle_battle_end(client_pid, iarg1);

        } else if (strcmp(cmd, "BUY") == 0) {
            token = strtok(NULL, ":"); if (!token) continue;
            iarg1 = atoi(token); // weapon index
            handle_buy_weapon(client_pid, iarg1);

        } else if (strcmp(cmd, "STATS") == 0) {
            handle_get_stats(client_pid);

        } else if (strcmp(cmd, "HISTORY") == 0) {
            handle_get_history(client_pid);

        } else if (strcmp(cmd, "BSTAT") == 0) {
            handle_get_battle_status(client_pid);
        }
    }

    return 0;
}
```

#### eternal.c
```
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/select.h>
#include "arena.h"
 
// variabel global
int    mq_id;
long   my_pid;
char   my_username[MAX_USERNAME];
int    my_gold, my_lvl, my_xp, my_weapon_dmg;
char   my_weapon[32];
 
// state battle
int    my_hp, my_max_hp;
int    opp_hp, opp_max_hp;
int    opp_lvl = 1;
char   opp_name[NAME_LEN];
int    battle_done     = 0;
int    battle_win      = 0;
int    is_bot          = 0;
int    bot_hp          = 100;
int    has_weapon      = 0;
 
// cooldown attack
volatile int atk_cooldown  = 0;
volatile int ult_cooldown  = 0;
 
// combat log (5 teratas)
#define MAX_LOG 5
char   combat_log[MAX_LOG][128];
int    log_count = 0;
 
// kirim pesan ke server
void send_to_server(const char *cmd) {
    Message msg;
    msg.mtype = 1; // server selalu listen di mtype=1
    snprintf(msg.mtext, sizeof(msg.mtext), "%ld:%s", my_pid, cmd);
    msgsnd(mq_id, &msg, sizeof(msg.mtext), 0);
}

// terima pesan dari server (blocking)
void recv_from_server(char *buf, size_t size) {
    Message msg;
    msgrcv(mq_id, &msg, sizeof(msg.mtext), my_pid, 0);
    strncpy(buf, msg.mtext, size - 1);
    buf[size - 1] = '\0';
}
 
// tambah combat log
void add_log(const char *entry) {
    if (log_count < MAX_LOG) {
        strncpy(combat_log[log_count], entry, 127);
        log_count++;
    } else {
        // geser ke atas, buang yang paling lama
        for (int i = 0; i < MAX_LOG - 1; i++)
            strcpy(combat_log[i], combat_log[i + 1]);
        strncpy(combat_log[MAX_LOG - 1], entry, 127);
    }
}

// baca satu karakter tanpa enter (non-blocking)
// terminal mode: set raw mode sekali saja
struct termios orig_termios;
 
void set_raw_mode() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}
 
void restore_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}
 
int kbhit() {
    char ch;
    int n = read(STDIN_FILENO, &ch, 1);
    if (n > 0) { ungetc(ch, stdin); return 1; }
    return 0;
}
 
char getch_noecho() {
    char ch;
    read(STDIN_FILENO, &ch, 1);
    return ch;
}
 
// tampilkan battle screen
void draw_battle() {
    // clear screen
    printf("\033[2J\033[H");
 
    printf("  .---------- ARENA ----------.\n");
    printf("\n");
 
    // lawan (atas)
    int opp_bar = opp_max_hp > 0 ? (opp_hp * 20 / opp_max_hp) : 0;
    printf("  %-15s Lvl %d\n", opp_name, opp_lvl);
    printf("  [");
    for (int i = 0; i < 20; i++) printf(i < opp_bar ? "#" : " ");
    printf("] %d/%d\n", opp_hp, opp_max_hp);
 
    printf("\n              VS\n\n");
 
    // kita (bawah)
    int my_bar = my_max_hp > 0 ? (my_hp * 20 / my_max_hp) : 0;
    printf("  %-15s Lvl %d | Weapon: %s\n", my_username, my_lvl, my_weapon);
    printf("  [");
    for (int i = 0; i < 20; i++) printf(i < my_bar ? "#" : " ");
    printf("] %d/%d\n", my_hp, my_max_hp);
 
    printf("\n  Combat Log:\n");
    for (int i = 0; i < MAX_LOG; i++) {
        if (i < log_count)
            printf("  > %s\n", combat_log[i]);
        else
            printf("  >\n");
    }
 
    printf("\n  CD: Atk(%ds) | Ult(%ds)\n", atk_cooldown, ult_cooldown);
    printf("  `-----------------------------`\n");
    printf("  [a] Attack  [u] Ultimate\n");
    fflush(stdout);
}
 
// thread untuk receive battle messages
void *battle_recv_thread(void *arg) {
    char buf[512];
    while (!battle_done) {
        Message msg;
        // non-blocking receive dengan timeout kecil
        struct timespec ts = {0, 100000000}; // 100ms
        nanosleep(&ts, NULL);
 
        if (msgrcv(mq_id, &msg, sizeof(msg.mtext), my_pid, IPC_NOWAIT) < 0)
            continue;
 
        strncpy(buf, msg.mtext, sizeof(buf) - 1);
 
        char log_entry[128];
 
        if (strncmp(buf, "HIT:", 4) == 0) {
            // kita kena serang: HIT:dmg:attacker:my_new_hp
            int dmg, new_hp;
            char attacker[NAME_LEN];
            sscanf(buf + 4, "%d:%31[^:]:%d", &dmg, attacker, &new_hp);
            my_hp = new_hp;
            snprintf(log_entry, sizeof(log_entry), "%s hit you for %d damage!", attacker, dmg);
            add_log(log_entry);
 
            if (my_hp <= 0) {
                battle_done = 1;
                battle_win  = 0;
            }
            draw_battle();
 
        } else if (strncmp(buf, "ATK:", 4) == 0) {
            // serangan kita berhasil: ATK:dmg:target (BOT) atau ATK:dmg:target:opp_new_hp (PvP)
            int dmg, new_opp_hp = -1;
            char target[NAME_LEN];
            sscanf(buf + 4, "%d:%31[^:]:%d", &dmg, target, &new_opp_hp);
            snprintf(log_entry, sizeof(log_entry), "You hit for %d damage!", dmg);
            add_log(log_entry);
 
            if (is_bot) {
                bot_hp -= dmg;
                if (bot_hp < 0) bot_hp = 0;
                opp_hp = bot_hp;
                if (bot_hp <= 0) {
                    battle_done = 1;
                    battle_win  = 1;
                }
            } else if (new_opp_hp >= 0) {
                // update HP lawan dari data server
                opp_hp = new_opp_hp;
                if (opp_hp <= 0) {
                    battle_done = 1;
                    battle_win  = 1;
                }
            }
            draw_battle();
 
        } else if (strncmp(buf, "END:", 4) == 0) {
            // battle selesai dari server
            int win, xp, gold, lvl;
            sscanf(buf + 4, "%d:%d:%d:%d", &win, &xp, &gold, &lvl);
            battle_win  = win;
            my_xp      += xp;
            my_gold    += gold;
            my_lvl      = lvl;
            battle_done = 1;
        }
    }
    return NULL;
}
 
// thread untuk cooldown timer
void *cooldown_thread(void *arg) {
    while (!battle_done) {
        sleep(1);
        if (battle_done) break;  // cek lagi setelah sleep
        if (atk_cooldown > 0) { atk_cooldown--; draw_battle(); }
        if (ult_cooldown > 0) { ult_cooldown--; draw_battle(); }
    }
    return NULL;
}
 
// layar battle utama
void do_battle(const char *opponent) {
    // Reset state
    battle_done = 0;
    battle_win  = 0;
    log_count   = 0;
    atk_cooldown = 0;
    ult_cooldown = 0;
    memset(combat_log, 0, sizeof(combat_log));
 
    strncpy(opp_name, opponent, NAME_LEN - 1);
    is_bot = (strcmp(opponent, "BOT") == 0);
 
    // ambil battle status awal dari server
    send_to_server("BSTAT");
    char buf[512];
    recv_from_server(buf, sizeof(buf));
    // BSTAT:my_hp:my_max_hp:opp_name:opp_hp:opp_max_hp:opp_lvl
    int opp_cur_hp = 0, opp_cur_max_hp = 0;
    sscanf(buf + 6, "%d:%d:%31[^:]:%d:%d:%d",
           &my_hp, &my_max_hp, opp_name, &opp_cur_hp, &opp_cur_max_hp, &opp_lvl);
    opp_hp     = opp_cur_hp;
    opp_max_hp = opp_cur_max_hp;  // pakai max HP yang dikirim server, bukan HP saat ini
 
    if (is_bot) {
        bot_hp     = 100;
        opp_hp     = 100;
        opp_max_hp = 100;
        my_max_hp  = BASE_HEALTH + (my_xp / 10);
        my_hp      = my_max_hp;
        strcpy(opp_name, "Wild Beast");
    }
 
    // hitung damage kita
    int my_dmg = BASE_DAMAGE + (my_xp / 50) + my_weapon_dmg;
 
    // start threads
    pthread_t recv_tid, cd_tid;
    pthread_create(&recv_tid, NULL, battle_recv_thread, NULL);
    pthread_create(&cd_tid,   NULL, cooldown_thread,    NULL);
    pthread_detach(cd_tid);
 
    set_raw_mode();
    draw_battle();
 
    // loop input battle
    while (!battle_done) {
        // pakai select() untuk cek apakah ada input, timeout 50ms
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000}; // 50ms
        int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) continue;
 
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;
 
        if ((c == 'a' || c == 'A') && atk_cooldown == 0) {
            atk_cooldown = ATTACK_COOLDOWN;
            send_to_server("ATTACK");
            if (is_bot) {
                int my_dmg2 = BASE_DAMAGE + (my_xp / 50) + my_weapon_dmg;
                bot_hp -= my_dmg2;
                if (bot_hp < 0) bot_hp = 0;
                opp_hp = bot_hp;
                char entry1[128];
                snprintf(entry1, sizeof(entry1), "You hit for %d damage!", my_dmg2);
                add_log(entry1);
                if (bot_hp <= 0) {
                    battle_done = 1;
                    battle_win  = 1;
                } else {
                    // bot attack balik secara random
                    int bot_dmg = rand() % 15 + 5;
                    my_hp -= bot_dmg;
                    if (my_hp < 0) my_hp = 0;
                    char entry2[128];
                    snprintf(entry2, sizeof(entry2), "Wild Beast hit you for %d damage!", bot_dmg);
                    add_log(entry2);
                    if (my_hp <= 0) {
                        battle_done = 1;
                        battle_win  = 0;
                    }
                }
            }
            draw_battle();
 
        } else if ((c == 'u' || c == 'U') && ult_cooldown == 0 && has_weapon) {
            ult_cooldown = ATTACK_COOLDOWN * 3;
            send_to_server("ULTIMATE");
            if (is_bot) {
                int ult_dmg = my_dmg * 3;
                bot_hp -= ult_dmg;
                if (bot_hp < 0) bot_hp = 0;
                opp_hp = bot_hp;
                char entry[128];
                snprintf(entry, sizeof(entry), "ULTIMATE! You hit for %d damage!", ult_dmg);
                add_log(entry);
                if (bot_hp <= 0) {
                    battle_done = 1;
                    battle_win  = 1;
                }
            }
            draw_battle();
        }
    }

    // pastikan semua thread tau battle sudah selesai
    battle_done = 1;
    pthread_join(recv_tid, NULL);
    restore_mode();
 
    // beritahu server battle selesai
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "BATTLE_END:%d", battle_win);
    send_to_server(cmd);
    recv_from_server(buf, sizeof(buf));
    // END:win:xp:gold:lvl
    int win, xp, gold, lvl;
    sscanf(buf + 4, "%d:%d:%d:%d", &win, &xp, &gold, &lvl);
    my_xp   = my_xp + xp;
    my_gold = my_gold + gold;
    my_lvl  = lvl;

    // tampilkan hasil
    printf("\033[2J\033[H");
    if (battle_win)
        printf("\n  === VICTORY ===\n");
    else
        printf("\n  === DEFEAT ===\n");
 
    printf("\n  Battle ended. Press [ENTER] to continue...");
    fflush(stdout);
 
    // flush stdin lalu tunggu enter
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF);
    getchar();
}
 
// menu matchmaking
void menu_matchmaking() {
    printf("\033[2J\033[H");
    printf("  Searching for an opponent... [0 s]\n");
    fflush(stdout);
 
    send_to_server("MATCHMAKING");
 
    char buf[512];
    char opponent[NAME_LEN] = "";
 
    while (1) {
        recv_from_server(buf, sizeof(buf));
 
        if (strncmp(buf, "WAIT:", 5) == 0) {
            int t = atoi(buf + 5);
            printf("\r  Searching for an opponent... [%d s]  ", t);
            fflush(stdout);
 
        } else if (strncmp(buf, "MATCH:", 6) == 0) {
            strncpy(opponent, buf + 6, NAME_LEN - 1);
            break;
 
        } else if (strncmp(buf, "ERR:", 4) == 0) {
            printf("\n  [Error] %s\n", buf + 4);
            return;
        }
    }
 
    printf("\n  Opponent found: %s!\n", opponent);
    sleep(1);
    do_battle(opponent);
}
 
// menu armory
void menu_armory() {
    char buf[512];
    while (1) {
        printf("\033[2J\033[H");
        printf("  === ARMORY ===\n");
        printf("  Gold: %d\n", my_gold);
        for (int i = 0; i < MAX_WEAPONS; i++) {
            printf("  %d. %-15s | %4d G | +%d Dmg\n",
                   i + 1, WEAPONS[i].name, WEAPONS[i].price, WEAPONS[i].bonus_dmg);
        }
        printf("  0. Back | Choice: ");
        fflush(stdout);
 
        int choice;
        scanf("%d", &choice);
        while (getchar() != '\n');
 
        if (choice == 0) break;
        if (choice < 1 || choice > MAX_WEAPONS) {
            printf("  Pilihan tidak valid.\n");
            sleep(1);
            continue;
        }
 
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "BUY:%d", choice - 1);
        send_to_server(cmd);
        recv_from_server(buf, sizeof(buf));
 
        if (strncmp(buf, "OK:", 3) == 0) {
            printf("\n  %s\n", buf + 3);
            my_gold -= WEAPONS[choice - 1].price;
            // update weapon jika bonus lebih besar
            if (WEAPONS[choice - 1].bonus_dmg > my_weapon_dmg) {
                my_weapon_dmg = WEAPONS[choice - 1].bonus_dmg;
                strncpy(my_weapon, WEAPONS[choice - 1].name, 31);
                has_weapon = 1;
            }
        } else {
            printf("\n  [Error] %s\n", buf + 4);
        }
 
        // refresh gold dari server
        send_to_server("STATS");
        recv_from_server(buf, sizeof(buf));
        if (strncmp(buf, "STATS:", 6) == 0) {
            sscanf(buf + 6, "%d:%d:%d:%d:%31s",
                   &my_gold, &my_lvl, &my_xp, &my_weapon_dmg, my_weapon);
            has_weapon = (my_weapon_dmg > 0);
        }
 
        sleep(1);
    }
}
 
// menu match history
void menu_history() {
    send_to_server("HISTORY");
 
    printf("\033[2J\033[H");
    printf("  === MATCH HISTORY ===\n\n");
    printf("  %-8s %-15s %-6s %-6s\n", "Time", "Opponent", "Res", "XP");
    printf("  %-8s %-15s %-6s %-6s\n", "----", "--------", "---", "--");
 
    char buf[512];
    int empty = 1;
    while (1) {
        recv_from_server(buf, sizeof(buf));
        if (strcmp(buf, "HIST:DONE") == 0) break;
        if (strcmp(buf, "HIST:EMPTY") == 0) {
            printf("  (Belum ada riwayat pertempuran)\n");
            empty = 0;
            break;
        }
        // HIST:time:opponent:WIN/LOSS:+XP
        char time[16], opp[NAME_LEN], res[8], xp[16];
        sscanf(buf + 5, "%15[^:]:%31[^:]:%7[^:]:%15s", time, opp, res, xp);
        printf("  %-8s %-15s %-6s %-6s\n", time, opp, res, xp);
        empty = 0;
    }
 
    printf("\n  Press enter...");
    fflush(stdout);
    getch_noecho();
}
 
// menu utama setelah login
void menu_main() {
    while (1) {
        // refresh stats dari server tiap kali masuk menu
        send_to_server("STATS");
        char sbuf[512];
        recv_from_server(sbuf, sizeof(sbuf));
        if (strncmp(sbuf, "STATS:", 6) == 0) {
            sscanf(sbuf + 6, "%d:%d:%d:%d:%31s",
            &my_gold, &my_lvl, &my_xp, &my_weapon_dmg, my_weapon);
            has_weapon = (my_weapon_dmg > 0);
        }

        printf("\033[2J\033[H");
        printf("  === BATTLE ETERION ===\n");
        printf("  User  : %s\n", my_username);
        printf("  Gold  : %d\n", my_gold);
        printf("  Lvl   : %d\n", my_lvl);
        printf("  XP    : %d\n", my_xp);
        printf("  Weapon: %s\n\n", my_weapon);
        printf("  1. Battle\n");
        printf("  2. Armory\n");
        printf("  3. Match History\n");
        printf("  4. Logout\n");
        printf("  Choice: ");
        fflush(stdout);
 
        int choice;
        scanf("%d", &choice);
        while (getchar() != '\n');
 
        if (choice == 1) {
            menu_matchmaking();
        } else if (choice == 2) {
            menu_armory();
        } else if (choice == 3) {
            menu_history();
        } else if (choice == 4) {
            send_to_server("LOGOUT");
            printf("\n  Goodbye, %s!\n", my_username);
            sleep(1);
            break;
        }
    }
}
 
// menu register
void menu_register() {
    char username[MAX_USERNAME], password[MAX_PASSWORD];
    char buf[512];
 
    printf("\033[2J\033[H");
    printf("  CREATE ACCOUNT\n");
    printf("  Username: ");
    fflush(stdout);
    scanf("%31s", username);
    while (getchar() != '\n');
 
    printf("  Password: ");
    fflush(stdout);
    scanf("%31s", password);
    while (getchar() != '\n');
 
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "REGISTER:%s:%s", username, password);
    send_to_server(cmd);
    recv_from_server(buf, sizeof(buf));
 
    if (strncmp(buf, "OK:", 3) == 0)
        printf("\n  %s\n", buf + 3);
    else
        printf("\n  [Error] %s\n", buf + 4);
 
    sleep(1);
}
 
// menu login
void menu_login() {
    char username[MAX_USERNAME], password[MAX_PASSWORD];
    char buf[512];
 
    printf("\033[2J\033[H");
    printf("  LOGIN\n");
    printf("  Username: ");
    fflush(stdout);
    scanf("%31s", username);
    while (getchar() != '\n');
 
    printf("  Password: ");
    fflush(stdout);
    scanf("%31s", password);
    while (getchar() != '\n');
 
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "LOGIN:%s:%s", username, password);
    send_to_server(cmd);
    recv_from_server(buf, sizeof(buf));
 
    if (strncmp(buf, "OK:", 3) == 0) {
        printf("\n  Welcome!\n");
        strncpy(my_username, username, MAX_USERNAME - 1);
        // OK:gold:lvl:xp:weapon_dmg:weapon_name
        sscanf(buf + 3, "%d:%d:%d:%d:%31s",
               &my_gold, &my_lvl, &my_xp, &my_weapon_dmg, my_weapon);
        has_weapon = (my_weapon_dmg > 0);
        if (strcmp(my_weapon, "None") != 0 && my_weapon_dmg > 0)
            has_weapon = 1;
        sleep(1);
        menu_main();
    } else {
        printf("\n  [Error] %s\n", buf + 4);
        sleep(1);
    }
}
 
// signal handler untuk logout bersih saat Ctrl+C
void handle_exit(int sig) {
    restore_mode();
    if (strlen(my_username) > 0)
        send_to_server("LOGOUT");
    printf("\n  [System] Disconnected.\n");
    exit(0);
}

// main
int main() {
    srand(time(NULL));
    my_pid = (long)getpid();
    signal(SIGINT, handle_exit);

    // menyoba connect ke message queue server
    mq_id = msgget(MQ_KEY, 0666);
    if (mq_id < 0) {
        printf("  Orion are you there?\n");
        exit(1);
    }

    // ASCII art judul
    printf("\033[2J\033[H");
    printf("    ____        __  __  __              ____   ________            _           \n");
    printf("   / __ )____ _/ /_/ /_/ /__     ____  / __/  / ____/ /____  _____(_)___  ____ \n");
    printf("  / __  / __ `/ __/ __/ / _ \\   / __ \\/ /_   / __/ / __/ _ \\/ ___/ / __ \\/ __ \\ \n");
    printf(" / /_/ / /_/ / /_/ /_/ /  __/  / /_/ / __/  / /___/ /_/  __/ /  / / /_/ / / / /\n");
    printf("/_____/\\__,_/\\__/\\__/_/\\___/   \\____/_/    /_____/\\__/\\___/_/  /_/\\____/_/ /_/ \n");
    printf("\n");

    while (1) {
        printf("  1. Register\n");
        printf("  2. Login\n");
        printf("  3. Exit\n");
        printf("  Choice: ");
        fflush(stdout);
 
        int choice;
        scanf("%d", &choice);
        while (getchar() != '\n');
 
        if (choice == 1) {
            menu_register();
        } else if (choice == 2) {
            menu_login();
        } else if (choice == 3) {
            printf("  Goodbye!\n");
            break;
        } else {
            printf("  Pilihan tidak valid.\n");
            sleep(1);
        }

        // tampilkan ulang ASCII art setelah menu
        printf("\033[2J\033[H");
        printf("    ____        __  __  __              ____   ________            _           \n");
        printf("   / __ )____ _/ /_/ /_/ /__     ____  / __/  / ____/ /____  _____(_)___  ____ \n");
        printf("  / __  / __ `/ __/ __/ / _ \\   / __ \\/ /_   / __/ / __/ _ \\/ ___/ / __ \\/ __ \\ \n");
        printf(" / /_/ / /_/ / /_/ /_/ /  __/  / /_/ / __/  / /___/ /_/  __/ /  / / /_/ / / / /\n");
        printf("/_____/\\__,_/\\__/\\__/_/\\___/   \\____/_/    /_____/\\__/\\___/_/  /_/\\____/_/ /_/ \n");
        printf("\n");
    }

    return 0;
}
```

### Cara Menjalankan Soal 2

**Kompilasi:**
```
make
# yang akan menjalankan command
gcc -Wall -pthread orion.c -o orion -lrt
gcc -Wall -pthread eternal.c -o eternal -lrt
```

**Menjalankan:**
```
# Terminal 1: jalankan server terlebih dahulu
./orion

# Terminal 2, 3, dst: jalankan client
./eternal
```

**Membersihkan IPC resource** (jika server crash dan resource tertinggal):
```bash
make clear_ipc
```

**Alur penggunaan:**
1. Pilih `Register` untuk membuat akun baru
2. Login dengan akun yang sudah dibuat
3. Di menu utama, pilih `Matchmaking` untuk mencari lawan
4. Saat battle: tekan `a` untuk Attack, `u` untuk Ultimate, `q` untuk menyerah
5. Setelah battle, XP dan gold diberikan sebagai reward

### Screenshot Soal 2
<img width="1057" height="331" alt="image" src="https://github.com/user-attachments/assets/e225c8bb-3f93-44ef-9cda-4ac900ae87f4" />
<img width="955" height="86" alt="image" src="https://github.com/user-attachments/assets/c5506254-fd2e-4c81-a27a-781eb0554ef9" />
<img width="987" height="266" alt="image" src="https://github.com/user-attachments/assets/f47176a0-568c-4d10-be00-086347968449" />
<img width="979" height="149" alt="image" src="https://github.com/user-attachments/assets/4414496a-5e2d-4f94-88bd-6e92295fcc05" />
<img width="965" height="636" alt="image" src="https://github.com/user-attachments/assets/d51b9622-7347-4399-9448-af31c4156956" />
<img width="509" height="225" alt="image" src="https://github.com/user-attachments/assets/d0826a3c-662d-40f5-8c42-48e0392efe7f" />
<img width="601" height="266" alt="image" src="https://github.com/user-attachments/assets/3e1c3815-1afd-4d56-9754-5faa63381a93" />
<img width="471" height="821" alt="image" src="https://github.com/user-attachments/assets/ff9b10ce-ae10-4450-88b1-dcdce6e8abf8" />
<img width="549" height="565" alt="image" src="https://github.com/user-attachments/assets/a47aea5e-fdb9-4665-8756-94691905d79c" />
<img width="1071" height="576" alt="image" src="https://github.com/user-attachments/assets/400db06e-7c38-4e40-adc0-42cc1133a6ab" />
<img width="1115" height="594" alt="image" src="https://github.com/user-attachments/assets/44f10e30-d211-4240-8219-8e85d5f9db7f" />
<img width="564" height="472" alt="image" src="https://github.com/user-attachments/assets/de5f1a38-014b-47f5-9b40-27d7c6446cf1" />

### Kendala Soal 2

Banyak sekali bug yang terjadi saat matchmakingnya, mulai dari sistem attacking dan ultimatenya, saving gold dan expnya, PVPnya, pembelian weapon, dan sistem history match. Matchmaking sangat sulit untuk diimplementasi mulai dari perlawanan dengan BOT yang saat pertama kalinya tidak bisa melakukan attack maupun ultimate, keduanya saat selesai match gold dan exp tidak terupdate dan tersave, kemudian saat sudah tersave tidak transfer memori gold saat itu ke dalam shop yang mengakibatkan gold static menjadi gold awal, terdapat bug saat mencoba PVP (terminal vs terminal) dimana 1 terminal tidak menampilkan hp masing-masing dan 1 terminal menampilkannya dengan normal, saat pvp juga misal saat 2 terminal berlawan dan match selesai (terminal 1 menang terminal 2 kalah) terminal 1 tetap stuck di dalam match sedangkan terminal 2 menampilkan screen defeat sesuai ekspektasi. Juga terdapat kendala saat pembuatan ASCII spesifik pada simbol '\' yang tidak terprint pada output.
