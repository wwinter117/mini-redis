#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <regex.h>

#define TABLE_SIZE 16
#define BUFFER_SIZE 1024
#define REDIS_DEFAULT_RDB_FILENAME "dump.rdb"
#define REDIS_DEFAULT_DBNUM 8
#define REDIS_SERVERPORT 6379


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

    redisdb rdb[REDIS_DEFAULT_DBNUM];
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
    for (int i = 0; i < REDIS_DEFAULT_DBNUM; ++i) {
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


/* ======================= send RESP data process ======================= */

void send_basic(const char *str, int flag) {
    int len = (int) strlen(str);
    char *response = (char *) malloc(len + 4); // +4 for -, \r, \n, \0
    if (!response) {
        send(rs.new_socket, "-ERR\r\n", 6, 0);
        return;
    }
    if (flag) {
        sprintf(response, "+%s\r\n", str);
    } else {
        sprintf(response, "-%s\r\n", str);
    }
    send(rs.new_socket, response, strlen(response), 0);
    free(response);
}

void send_error(const char *str) {
    send_basic(str, 0);
}

void send_ok(const char *str) {
    send_basic(str, 1);
}

void send_integer(int num) {
    char *response = (char *) malloc(24);
    if (!response) {
        send(rs.new_socket, "-ERR\r\n", 6, 0);
        return;
    }
    sprintf(response, ":%d\r\n", num);
    send(rs.new_socket, response, strlen(response), 0);
    free(response);
}

void send_bulk_string(const char *str) {
    int len = (int) strlen(str);
    if (!len) {
        send(rs.new_socket, "$-1\r\n", 7, 0);
        return;
    }
    char *response = (char *) malloc(len + 16); // +16 for $, length, \r, \n, \r, \n, \0
    if (!response) {
        send(rs.new_socket, "-ERR\r\n", 6, 0);
        return;
    }
    sprintf(response, "$%d\r\n%s\r\n", len, str);
    send(rs.new_socket, response, strlen(response), 0);
    free(response);
}

void send_array(char **elements, int count) {
    int total_len = 0;

    for (int i = 0; i < count; i++) {
        total_len += (int) strlen(elements[i]) + 5; // +5 for $, length, \r, \n, \r, \n
    }

    char *response = (char *) malloc(total_len + 16); // +16 for *, count, \r, \n, \0
    if (!response) {
        send(rs.new_socket, "-ERR\r\n", 6, 0);
        return;
    }
    char *ptr = response;
    ptr += sprintf(ptr, "*%d\r\n", count);

    for (int i = 0; i < count; i++) {
        int len = (int) strlen(elements[i]);
        ptr += sprintf(ptr, "$%d\r\n%s\r\n", len, elements[i]);
    }
    send(rs.new_socket, response, strlen(response), 0);
    free(response);
}

/* ======================= CMD ======================= */
void cmd_set(int argc, char *argv[]);

void cmd_get(int argc, char *argv[]);

void cmd_exp(int argc, char *argv[]);

void cmd_ttl(int argc, char *argv[]);

void cmd_save(int argc, char *argv[]);

void cmd_keys(int argc, char *argv[]);

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
        {"KEYS",   cmd_keys},
        {"SELECT", cmd_select},
};

/* ======================= REdis Serialization Protocol process ======================= */

/*
 * no ~
 */
void
handle_command(char *command) {
    printf("CMD: %s\n", command);

    int argc = 0;
    char *argv[10];

    if (command[0] != '*') {
        send_error("protocol error");
        return;
    }
    char *ptr = command + 1;
    argc = atoi(ptr);
    if (argc < 1 || argc > 10) {
        send_error("invalid number of arguments");
        return;
    }

    char *token = strtok(command, "\r\n");
    int i = 0;
    token += 4;
    while (token != NULL) {
        if (*token == '$') {
            token = strtok(NULL, "\r\n");
            continue;
        }
        argv[i++] = token;
        token = strtok(NULL, "\r\n");
    }
    argv[i] = NULL;
    if (i != argc) {
        printf("i=%d\n", i);
        printf("argc=%d\n", argc);
        send_error("Erro: *");
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
    send_error("not suport");
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
            put(rs.rdb[cur_rdb].ht, key, value);
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
    for (int i = 0; i < REDIS_DEFAULT_DBNUM; ++i) {
        rs.rdb[i].ht = createHt();
        rs.rdb[i].exp = createHt();
    }
    cur_rdb = 0;
    cur_ht = rs.rdb[cur_rdb].ht;
    cur_expht = rs.rdb[cur_rdb].exp;
    rs.connected = 0;
    rs.userdb = 1;
    rs.rdb_name = REDIS_DEFAULT_RDB_FILENAME;
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
    rs.address.sin_port = htons(REDIS_SERVERPORT);

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
        send_error("Usage: set 'key' 'value'");
        return;
    }
    put(cur_ht, argv[1], argv[2]);
    send_ok("OK");
}

void cmd_get(int argc, char *argv[]) {
    if (argc != 2) {
        send_error("Usage: get 'key'");
    }
    char *value = get(cur_ht, argv[1]);
    if (value) {
        send_bulk_string(value);
    } else {
        send_bulk_string("");
    }
}

void cmd_exp(int argc, char *argv[]) {
    if (argc < 3) {
        send_error("Usage: EXPIRE 'key' 'time'");
        return;
    }
    if (expire(argv[1], argv[2])) {
        send_ok("OK");
    } else {
        send_error("key not exits");
    }
}

void cmd_ttl(int argc, char *argv[]) {
    if (argc != 2) {
        send_error("Usage: TTL 'key'");
        return;
    }
    char *value = get(cur_expht, argv[1]);
    if (value) {
        send_bulk_string(value);
    } else {
        send_bulk_string("");
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
        send_error("internal error");
        close(rs.rdb_fd);
    }
    free(buf);
}

void cmd_save(int argc, char *argv[]) {
    if ((rs.rdb_fd = open(rs.rdb_name, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) == -1) {
        send_error("internal error");
        perror("Unable to open rdb file for writing");
        exit(EXIT_FAILURE);
    }

    if (argc != 1 || strcmp(argv[0], "SAVE") != 0) {
        send_error("Usage: SAVE");
    }
    char h_buf[10] = "REDIS1.0.0";
    if ((write(rs.rdb_fd, h_buf, 10) != 10)) {
        send_error("internal error");
        close(rs.rdb_fd);
        return;
    }
    // 遍历所有数据库，保存已有数据
    for (int i = 0; i < REDIS_DEFAULT_DBNUM && rs.rdb[i].ht->size; ++i) {
        char db_buf[16] = {0};
        snprintf(db_buf, sizeof(db_buf), "DB%d", i);
        ssize_t len = (ssize_t) strlen(db_buf);
        if ((write(rs.rdb_fd, db_buf, len) != len)) {
            send_error("internal error");
            close(rs.rdb_fd);
            return;
        }
        hashtable *ht = rs.rdb[i].ht;
        printht(ht);
        for (int j = 0; j < TABLE_SIZE; ++j) {
            entry *e = ht->entry[j];
            while (e) {
                saveStrPair(e);
                e = e->next;
            }
        }
    }
    close(rs.rdb_fd);
    send_ok("OK");
}

void escape_regex_specials(const char *pattern, char *buf) {
    char *p = buf;
    while (*pattern) {
//        if (strchr(".*+?^$[](){}|\\", *pattern)) {
        if (strchr("*", *pattern)) {
            *p++ = '.';
        }
        *p++ = *pattern++;
    }
    *p = '\0';
}

void cmd_keys(int argc, char *argv[]) {
    if (argc != 2) {
        send_error("Usage: KEYS [reg]");
        return;
    }
    char buf[256];
    escape_regex_specials(argv[1], buf);
    regex_t regex;
    if (regcomp(&regex, buf, REG_EXTENDED)) {
        send_error("regex error");
        return;
    }
    char *keys[50];
    int j = 0;
    for (int i = 0; i < TABLE_SIZE; ++i) {
        entry *e = cur_ht->entry[i];
        while (e) {
            if (!regexec(&regex, e->key, 0, NULL, 0)) {
                keys[j++] = strdup(e->key);
            }
            e = e->next;
        }
    }
    regfree(&regex);
    send_array(keys, j);
}

void cmd_select(int argc, char *argv[]) {
    if (argc != 2) {
        send_error("Usage: SELECT [dbnum]");
        return;
    }
    cur_ht = rs.rdb[atoi(argv[1])].ht;
    send_ok("OK");
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
