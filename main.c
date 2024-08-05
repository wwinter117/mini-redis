#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define TABLE_SIZE 16
#define MAX_RDB_NUM 8
#define PORT 6379
#define BUFFER_SIZE 1024

/* hashentry data structure */
typedef struct entry {
    void *key;
    void *value;
    struct entry *next;
} entry;

/* hashtable data structure */
typedef struct hashtable {
    entry *entry[TABLE_SIZE];
    unsigned long size;
    unsigned long used;
} hashtable;

typedef struct redisdb {
    hashtable *ht;
    hashtable *exp;
} redisdb;

typedef struct redisserver {
    int connected;

    int userdb; // 1 - use rdb; 0 - not use rdb
    int appendonly; // 1 - use aof; 0 - not use aof

    int server_fd;
    int new_socket;

    struct sockaddr_in address;
    int addrlen;

    redisdb rdb[MAX_RDB_NUM];
} redisserver;

redisserver rs;
int cur_rdb;
hashtable *cur_ht;
hashtable *cur_expht;


/* ======================= Hashtable implementation ======================= */

unsigned int
hash(const char *key) {
    unsigned long int value = 0;
    unsigned int i = 0;
    unsigned int key_len = strlen(key);

    for (; i < key_len; ++i) {
        value = value * 37 + key[i];
    }
    value = value % TABLE_SIZE;
    return value;
}

entry *
createEntry(const char *key, const char *value) {
    entry *e = malloc(sizeof(entry));
    if (e == NULL) {
        perror("entry malloc");
        exit(EXIT_FAILURE);
    }
    if ((e->key = malloc(strlen(key) + 1)) == NULL) {
        perror("key malloc");
        exit(EXIT_FAILURE);
    }
    if ((e->value = malloc(strlen(value) + 1)) == NULL) {
        perror("value malloc");
        exit(EXIT_FAILURE);
    }
    strcpy(e->key, key);
    strcpy(e->value, value);
    e->next = NULL;
    return e;
}

void
freeEntry(entry *e) {
    free(e->key);
    free(e->value);
    free(e);
}

hashtable *
createHt(void) {
    hashtable *ht = malloc(sizeof(hashtable));
    if (ht == NULL) {
        perror("cur_ht malloc");
        exit(EXIT_FAILURE);
    }
    for (int j = 0; j < TABLE_SIZE; ++j) {
        ht->entry[j] = NULL;
    }
    ht->size = 0;
    ht->used = 0;
    return ht;
}

void
freeHt(hashtable *ht) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        entry *e = ht->entry[i];
        while (e) {
            entry *t = e;
            freeEntry(t);
            e = e->next;
        }
    }
    free(ht);
}

void
free_server(void) {
    for (int i = 0; i < MAX_RDB_NUM; ++i) {
        hashtable *ht = rs.rdb[i].ht;
        hashtable *expht = rs.rdb[i].exp;
        if (ht) {
            freeHt(ht);
        }
        if (expht) {
            freeHt(expht);
        }
    }
}

/* ======================= For debug ======================= */
void
printht(void) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        entry *e = cur_ht->entry[i];
        if (e) {
            printf("%d\t: ", i);
            while (e) {
                printf("(%s: %s)", (char *) e->key, (char *) e->value);
                if (e->next) {
                    printf("->");
                }
                e = e->next;
            }
            printf("\n");
        } else {
            printf("%d\t: NULL\n", i);
        }
    }
}

/* ======================= Data process ======================= */

void
put(hashtable *ht, const char *key, const char *value) {
    unsigned int slot = hash(key);
    entry *e = ht->entry[slot];
    if (e == NULL) {
        ht->entry[slot] = createEntry(key, value);
        return;
    }

    entry *pre = NULL;
    while (e) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            if ((e->value = malloc(strlen(value) + 1)) == NULL) {
                perror("value malloc");
                exit(EXIT_FAILURE);
            }
            strcpy(e->value, value);
            return;
        }
        pre = e;
        e = e->next;
    }
    pre->next = createEntry(key, value);
}

char *
get(hashtable *ht, const char *key) {
    unsigned int slot = hash(key);
    entry *e = ht->entry[slot];
    while (e) {
        if (strcmp(e->key, key) == 0) {
            return e->value;
        }
        e = e->next;
    }
    return NULL;
}

int
expire(char *key, char *time) {
    if (get(cur_ht, key) == NULL) {
        return 0;
    }
    put(cur_expht, key, time);
    return 1;
}

void
expireifneed(void) {
}

void
activeexpirecicle(void) {
}


/*
 * flag 0 - response error
 * flag 1 - response sucsess
 * for simple string response
 */
void
send2Client(char *response, int flag) {
    char res[BUFFER_SIZE];
    if (flag == 0) {
        snprintf(res, BUFFER_SIZE, "- %s\r\n", response);
    } else {
        snprintf(res, BUFFER_SIZE, "+ %s\r\n", response);
    }
    send(rs.new_socket, res, strlen(res), 0);
}


/* ======================= CMD ======================= */
void cmd_set(int argc, char *argv[]);

void cmd_get(int argc, char *argv[]);

void cmd_exp(int argc, char *argv[]);

void cmd_ttl(int argc, char *argv[]);

void cmd_save(int argc, char *argv[]);

static struct {
    char *name;

    void (*func)(int argc, char *argv[]);
} cmds[] = {
        {"SET",    cmd_set},
        {"GET",    cmd_get},
        {"EXPIRE", cmd_exp},
        {"TTL",    cmd_ttl},
        {"SAVE",   cmd_save},
};

/* ======================= REdis Serialization Protocol process ======================= */

/*
 * no ~
 */
void
handle_command(char *command) {
    int argc = 0;
    char *argv[10];

    if (command[0] != '*') {
        send2Client("protocol error", 0);
        return;
    }
    char *ptr = command + 1;
    argc = atoi(ptr);
    if (argc < 1 || argc > 10) {
        send2Client("invalid number of arguments", 0);
        return;
    }

    char *token = strtok(command, "\r\n");
    int i = 0;
    while (token != NULL) {
        if (*token == '*' || *token == '$') {
            token = strtok(NULL, "\r\n");
            continue;
        }
        argv[i++] = token;
        token = strtok(NULL, "\r\n");
    }
    argv[i] = NULL;
    if (i != argc) {
        send2Client("Erro: *", 0);
        return;
    }

    if (argc < 2) {
        send2Client("empty command", 0);
        return;
    }

    char **p = argv;
    printf("*Recived cmd:");
    while (*p) {
        printf(" %s", *p);
        p++;
    }
    printf("\n");

    for (int j = 0; j < sizeof(cmds) / sizeof(cmds[0]); ++j) {
        if (strcmp(argv[0], cmds[j].name) == 0) {
            cmds[j].func(argc, argv);
            return;
        }
    }
    send2Client("not suport", 0);
}

/* ======================= Pre work ======================= */

/*
 * initail rdb hastable and rdb expire hashtable
 */
void
serverinit(void) {
    for (int i = 0; i < MAX_RDB_NUM; ++i) {
        rs.rdb[i].ht = createHt();
        rs.rdb[i].exp = createHt();
    }
    cur_rdb = 0;
    cur_ht = rs.rdb[cur_rdb].ht;
    cur_expht = rs.rdb[cur_rdb].exp;
    rs.connected = 0;
    rs.userdb = 1;
    rs.appendonly = 0;
}

/*
 * bind socket and lesten on it
 */
void
networkinit(void) {
    rs.addrlen = sizeof(rs.address);

    if ((rs.server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    rs.address.sin_family = AF_INET;
    rs.address.sin_addr.s_addr = INADDR_ANY;
    rs.address.sin_port = htons(PORT);

    if (bind(rs.server_fd, (struct sockaddr *) &rs.address, sizeof(rs.address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(rs.server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
}

void
waitingForConne(void) {
    printf("[Mini-Redis] Waiting for connections...\n");
    if ((rs.new_socket = accept(rs.server_fd, (struct sockaddr *) &rs.address, (socklen_t *) &rs.addrlen)) < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }
    rs.connected = 1;
    printf("[Mini-Redis] Connection established\n");
}

void
processFileEvents(void) {
    // connected and handle every cmd
    char buffer[BUFFER_SIZE] = {0};
    memset(buffer, 0, BUFFER_SIZE);
    int valread = (int) read(rs.new_socket, buffer, BUFFER_SIZE);
    if (valread <= 0) {
        printf("[Mini-Redis] Connection closed\n");
        close(rs.new_socket);
        rs.connected = 0;
        return;
    }
    handle_command(buffer);
}

void
processTimeEvents(void) {

}

void
flushAppendOnlyFile(void) {

}


/* ======================= implimentations ======================= */

void cmd_set(int argc, char *argv[]) {
    if (argc < 3) {
        send2Client("Usage: set 'key' 'value'", 0);
        return;
    }
    put(cur_ht, argv[1], argv[2]);
    send2Client("OK", 1);
}

void cmd_get(int argc, char *argv[]) {
    if (argc != 2) {
        send2Client("Usage: get 'key'", 0);
        return;
    }
    char *value = get(cur_ht, argv[1]);
    if (value) {
        send2Client(value, 1);
    } else {
        send2Client("NULL", 0);
    }
}

void cmd_exp(int argc, char *argv[]) {
    if (argc < 3) {
        send2Client("Usage: EXPIRE 'key' 'time'", 0);
        return;
    }
    if (expire(argv[1], argv[2])) {
        send2Client("OK", 1);
    } else {
        send2Client("key not exits", 0);
    }
}

void cmd_ttl(int argc, char *argv[]) {
    if (argc != 2) {
        send2Client("Usage: TTL 'key'", 0);
        return;
    }
    char *value = get(cur_expht, argv[1]);
    if (value) {
        send2Client(value, 1);
    } else {
        send2Client("NULL", 0);
    }
}

void cmd_save(int argc, char *argv[]) {

}

int
main(int argc, char *argv[]) {
    if (argc != 1) {
        fprintf(stderr, "Usage: mini-redis-server\n");
        exit(EXIT_FAILURE);
    }

    printf("[Mini-Redis] is starting...\n");
    serverinit();
    networkinit();
    printf("[Mini-Redis] started\n");

    while (1) {
        // stop here for watting connetion...
        waitingForConne();

        while (rs.connected) {
            processFileEvents();
            processTimeEvents();
            flushAppendOnlyFile();
        }
    }
    free_server();
    return 0;
}
