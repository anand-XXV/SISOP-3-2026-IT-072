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