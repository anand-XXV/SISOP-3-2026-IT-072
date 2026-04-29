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