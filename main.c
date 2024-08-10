#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>

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
} hashtable;

typedef struct redisdb {
    hashtable *ht;
    hashtable *exp;
} redisdb;

typedef struct redisserver {
    int connected;

    int userdb; // 1 - use rdb; 0 - not use rdb
    int rdb_fd;
    char *rdb_name;

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
    if (!e) {
        perror("malloc failed for entry");
        exit(EXIT_FAILURE);
    }
    e->key = strdup(key);
    e->value = strdup(value);
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
    if (!ht) {
        perror("malloc failed for hashtable");
        exit(EXIT_FAILURE);
    }
    memset(ht->entry, 0, sizeof(ht->entry));
    ht->size = 0;
    return ht;
}

void
freeHt(hashtable *ht) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        entry *e = ht->entry[i];
        while (e) {
            entry *t = e;
            e = e->next;
            freeEntry(t);
        }
    }
    free(ht);
}

void
free_server(void) {
    for (int i = 0; i < MAX_RDB_NUM; ++i) {
        freeHt(rs.rdb[i].ht);
        freeHt(rs.rdb[i].exp);
    }
}


/* ======================= For debug ======================= */
void
printht(hashtable *ht) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        entry *e = ht->entry[i];
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
        ht->size++;
        return;
    }

    entry *pre = NULL;
    while (e) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = strdup(value);
            return;
        }
        pre = e;
        e = e->next;
    }
    pre->next = createEntry(key, value);
    ht->size++;
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
 * flag 2 - response string data
 * for simple string response
 */
void
send2Client(char *response, int flag) {
    char res[BUFFER_SIZE];
    if (flag == 0) {
        snprintf(res, BUFFER_SIZE, "-%s\r\n", response);
    } else if (flag == 1) {
        snprintf(res, BUFFER_SIZE, "+%s\r\n", response);
    } else {
        snprintf(res, BUFFER_SIZE, "$%lu\r\n%s\r\n", strlen(response), response);
    }
    send(rs.new_socket, res, strlen(res), 0);
}


/* ======================= CMD ======================= */
void cmd_set(int argc, char *argv[]);

void cmd_get(int argc, char *argv[]);

void cmd_exp(int argc, char *argv[]);

void cmd_ttl(int argc, char *argv[]);

void cmd_save(int argc, char *argv[]);

void cmd_select(int argc, char *argv[]);

static struct {
    char *name;

    void (*func)(int argc, char *argv[]);
} cmds[] = {
        {"SET",    cmd_set},
        {"GET",    cmd_get},
        {"EXPIRE", cmd_exp},
        {"TTL",    cmd_ttl},
        {"SAVE",   cmd_save},
        {"SELECT", cmd_save},
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

    char **p = argv;
    printf("*Recived cmd:");
    while (*p) {
        printf(" %s", *p);
        p++;
    }
    printf("\n");

    int num = sizeof(cmds) / sizeof(cmds[0]);
    for (int j = 0; j < num; ++j) {
        if (strcmp(argv[0], cmds[j].name) == 0) {
            cmds[j].func(argc, argv);
            return;
        }
    }
    send2Client("not suport", 0);
}

/* ======================= Pre work ======================= */

/*
 * 解析数据库和键值对
 */
int parseRdbContent(const unsigned char *data, size_t size) {
    size_t offset = 10;  // Skip "REDIS" and version
    printf("[Mini-Redis] start recovery\n");

    while (offset < size) {
        if (strncmp((const char *) (data + offset), "DB", 2) == 0) {
            cur_rdb = data[offset + 2] - '0';
            printf("[Mini-Redis] found database: %d\n", cur_rdb);
            offset += 3;
        } else {
            int key_len = data[offset++] - '0';
            char *key = strndup((const char *) (data + offset), key_len);
            offset += key_len;

            int value_len = data[offset++] - '0';
            char *value = strndup((const char *) (data + offset), value_len);
            offset += value_len;

            printf("found: %s-%s\n", key, value);
            put(cur_ht, key, value);
            free(key);
            free(value);
        }
    }
    printf("[Mini-Redis] done\n");
    return 1;
}

void recoverRdb(void) {
    FILE *file = fopen(rs.rdb_name, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open rdb file\n");
        return;
    }
    // 文件大小
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // 读取文件内容
    unsigned char *data = malloc(size);
    if (!data) {
        fclose(file);
        fprintf(stderr, "Failed to allocate memory\n");
        return;
    }

    fread(data, 1, size, file);
    fclose(file);

    // 解析 RDB 文件头
    if (strncmp((const char *) data, "REDIS", 5) != 0) {
        fprintf(stderr, "Not a valid Redis RDB file.\n");
        free(data);
        return;
    }
    char version[6] = {0};
    strncpy(version, (const char *) (data + 5), 5); // 提取版本号
    printf("[Mini-Redis] RDB Version: %s\n", version);

    // 解析 RDB 键值对
    parseRdbContent(data, size);
    free(data);
}

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
    rs.rdb_name = "dump.rdb";
    recoverRdb();
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

/*
 * 阻塞io
 */
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
    if (argc != 3) {
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
        send2Client(value, 2);
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

void saveStrPair(entry *e) {
    char *k = e->key;
    char *v = e->value;
    size_t buf_size = strlen(k) + strlen(v) + 16;
    char *buf = malloc(buf_size);
    snprintf(buf, buf_size, "%d%s%d%s", (int) strlen(k), k, (int) strlen(v), v);
    printf("buf: %s\n", buf);
    ssize_t len = (ssize_t) strlen(buf);
    if ((write(rs.rdb_fd, buf, len)) != len) {
        send2Client("Save failed: entry", 0);
        close(rs.rdb_fd);
    }
    free(buf);
}

void cmd_save(int argc, char *argv[]) {
    if ((rs.rdb_fd = open(rs.rdb_name, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) == -1) {
        perror("Unable to open rdb file for writing");
        exit(EXIT_FAILURE);
    }

    if (argc != 1 || strcmp(argv[0], "SAVE") != 0) {
        send2Client("Usage: SAVE", 0);
    }
    char h_buf[10] = "REDIS1.0.0";
    if ((write(rs.rdb_fd, h_buf, 10) != 10)) {
        send2Client("Save failed: REDIS-version", 0);
        close(rs.rdb_fd);
        return;
    }
    // 遍历所有数据库，保存已有数据
    for (int i = 0; i < MAX_RDB_NUM && rs.rdb[i].ht->size; ++i) {
        char db_buf[16] = {0};
        snprintf(db_buf, sizeof(db_buf), "DB%d", i);
        ssize_t len = (ssize_t) strlen(db_buf);
        if ((write(rs.rdb_fd, db_buf, len) != len)) {
            send2Client("Save failed: DB", 0);
            close(rs.rdb_fd);
            return;
        }
        hashtable *ht = rs.rdb[i].ht;
        printht(ht);
        for (int j = 0; j < TABLE_SIZE; ++j) {
            entry *e = ht->entry[j];
            while (e) {
                printf("hi %d\n", j);
                saveStrPair(e);
                e = e->next;
            }
        }
    }
    close(rs.rdb_fd);
    send2Client("OK", 1);
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
