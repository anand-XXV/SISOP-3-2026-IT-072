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