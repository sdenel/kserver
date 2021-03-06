/** 
 * Implementation of kclient.h
 *
 * (c) Koheron
 */

#include "kclient.h"
#include "definitions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

/*
 * --------- Data structures ---------
 */
 
 int get_socket_fd(struct kclient *kcl)
 {
    int fd;
    
#if defined (__linux__)
    if (kcl->conn_type == TCP)
#endif
        fd = kcl->sockfd;
#if defined (__linux__)
    else // Unix socket
        fd = kcl->unix_sockfd;
#endif

    return fd;
 }

#ifdef DEBUG_KOHERON_API
void debug_display_kclient(struct kclient *kcl)
{
    int i,j;
    
    for (i=0; i<kcl->devs_num; i++) {
        printf("%s:\n", kcl->devices[i].name);
        printf("  ID: %i\n", kcl->devices[i].id);
        
        if (kcl->devices[i].ops_num > 0) {
            printf("  Operations:\n");
        
            for (j=0; j<kcl->devices[i].ops_num; j++) {
                printf("    - %s (%i)\n", 
                       kcl->devices[i].ops[j].name, 
                       kcl->devices[i].ops[j].id);
            }
        }
    }
}
#else
void debug_display_kclient(struct kclient *kcl)
{}
#endif

dev_id_t get_device_id(struct kclient *kcl, char *dev_name)
{
    int i;
    
    for (i=0; i<kcl->devs_num; i++)
        if (strcmp(kcl->devices[i].name, dev_name) == 0)
            return kcl->devices[i].id;

    return -1;
}

op_id_t get_op_id(struct kclient *kcl, dev_id_t dev_id, char *op_name)
{
    int i, j;
    
    for (i=0; i<kcl->devs_num; i++)
        if (kcl->devices[i].id == dev_id)
            for (j=0; j<kcl->devices[i].ops_num; j++)
                if (strcmp(kcl->devices[i].ops[j].name, op_name) == 0)
                    return kcl->devices[i].ops[j].id;
    
    return -1;
}

/**
 * @brief Add an operation to a device
 */
static void append_op_to_dev(struct device *dev, struct operation *op)
{
    strcpy(dev->ops[dev->ops_num].name, op->name);
    dev->ops[dev->ops_num].id = dev->ops_num;
    dev->ops_num++;
}

# if 0
/**
 * @brief Get an operation from the device @dev
 * @op_num Operation ID
 * @return A pointer to the operation, NULL if the operation ID doesn't exist
 */
struct operation * get_op(struct device * dev, int op_id)
{
    if(op_id >= dev->ops_num) {
        return NULL;
    }
    
    return &(dev->ops[op_id]);
}
#endif

/* 
 *  --------- Receive/Send ---------
 */
 
struct command* init_command(int dev_id)
{
    struct command *cmd = malloc(sizeof(*cmd));
    
    if (cmd == NULL) {
        fprintf(stderr, "Can't allocate command memory\n");
        return NULL;
    }
    
    cmd->dev_id = dev_id;
    cmd->op_ref = 0;
    cmd->params_num = 0;
    
    return cmd;
}

int add_parameter(struct command *cmd, long param)
{
    if (cmd->params_num >= MAX_PARAMS_NUM) {
        DEBUG_MSG("Command parameters overflow\n");
        return -1;
    }
    
    (cmd->params)[cmd->params_num] = param;
    cmd->params_num++;    
    
    return 0;
}

void free_command(struct command *cmd)
{
    if (cmd != NULL) {
        free(cmd);
        cmd = NULL;
    }
}

#define CMD_LEN 1024

#define CHECK_FORMAT(ret)               \
  if ((ret) < 0) {                      \
      DEBUG_MSG("Format error\n");      \
      return -1;                        \
  }                                     \
                                        \
  if ((ret) >= CMD_LEN) {               \
      DEBUG_MSG("Buffer overflow\n");   \
      return -1;                        \
  }

static int build_command_string(struct command *cmd, char *cmd_str)
{
    int i;    
    int ret = snprintf(cmd_str, CMD_LEN, "%i|%i|", 
                       cmd->dev_id, cmd->op_ref);          
    CHECK_FORMAT(ret)
    
    for (i=0; i<cmd->params_num; i++) {
        ret = snprintf(cmd_str + strlen(cmd_str), CMD_LEN, "%lu|", 
                       (cmd->params)[i]);
        CHECK_FORMAT(ret)
    }
    
    ret = snprintf(cmd_str+strlen(cmd_str), CMD_LEN, "\n");
    CHECK_FORMAT(ret)
    
    return 0;
}

int kclient_send(struct kclient *kcl, struct command *cmd)
{
    char cmd_str[CMD_LEN];
    
    if (build_command_string(cmd, cmd_str) < 0) {
        return -1;
    }
    
    if (write(get_socket_fd(kcl), cmd_str, strlen(cmd_str)) < 0) {
        fprintf(stderr, "Can't send command to KServer\n");
        return -1;
    }
    
    return 0;
}

void set_rcv_buff(struct rcv_buff *rcv_buff)
{
    memset(rcv_buff->buffer, 0, sizeof(char)*RCV_BUFFER_LEN);
    (rcv_buff->buffer)[0] = '\0';
    
    rcv_buff->max_buff_len = RCV_BUFFER_LEN;
    rcv_buff->current_len = 0;
}

#define RCV_LEN 2048

// TODO Add timeout 
int kclient_rcv_esc_seq(struct kclient *kcl, 
                        struct rcv_buff *rcv_buffer, char *esc_seq)
{
    int bytes_rcv = 0;
    int bytes_read = 0;
    int num_dev = 0;
    
    char tmp_buff[RCV_LEN];
    
    set_rcv_buff(rcv_buffer);
    
    while(1) {
        int i;        
        tmp_buff[0] = '\0';
                        
        bytes_rcv = recv(get_socket_fd(kcl), tmp_buff, RCV_LEN-1, 0);
        
        if (bytes_rcv == 0) {
            fprintf(stderr, "Connection closed by KServer\n");
            return -1;
        }
        
        if (bytes_rcv < 0) {
            fprintf(stderr, "Can't receive data\n");
            return -1;
        }
        
        if (bytes_read + bytes_rcv >= rcv_buffer->max_buff_len) {
            DEBUG_MSG("Buffer overflow\n");
            return -1;
        }

        // Eliminates null-termination characters and 
        // concatenate result into rcv_buffer
        for (i=0; i<bytes_rcv; i++) {            
            if (tmp_buff[i] != '\0') {
                (rcv_buffer->buffer)[bytes_read] = tmp_buff[i];
                bytes_read++;
            } else if (tmp_buff[i] != '\n') {
                num_dev++;
            }
        }
        
        (rcv_buffer->buffer)[bytes_read] = '\0';
        rcv_buffer->current_len = bytes_read + 1;
        
        if (strstr(rcv_buffer->buffer, esc_seq)) {
            break;
        }
    }
    
    // XXX It's a bit ugly to have this here !!
    // This is specific to kclient initialization
    // but this function can be used to other purpose !
    kcl->devs_num = num_dev-2;
    
    if (kcl->devs_num > MAX_DEV_NUM) {
        DEBUG_MSG("MAX_DEV_NUM overflow\n");
        return -1;
    }
    
    return bytes_read;
}

#if 0
int kclient_rcv_n_bytes(struct kclient *kcl, char *rcv_buffer, int n_bytes)
{
    int bytes_rcv = 0;
    int bytes_read = 0;
    
    char tmp_buff[RCV_LEN];
    
    memset(rcv_buffer, 0, sizeof(char)*RCV_BUFFER_LEN);
    rcv_buffer[0] = '\0';
    
    while(1) {
        int i;        
        tmp_buff[0] = '\0';
                        
        bytes_rcv = recv(kcl->sockfd, tmp_buff, RCV_LEN-1, 0);
        
        if(bytes_rcv == 0) {
            fprintf(stderr, "Connection closed by KServer\n");
            return -1;
        }
        
        if(bytes_rcv < 0) {
            fprintf(stderr, "Can't receive data\n");
            return -1;
        }
        
        if(bytes_read + bytes_rcv >= RCV_BUFFER_LEN) {
            DEBUG_MSG("Buffer overflow\n");
            return -1;
        }
    }
}
#endif

int kclient_send_string(struct kclient *kcl, const char *str)
{
    if (write(get_socket_fd(kcl), str, strlen(str)) < 0) {
        fprintf(stderr, "Can't send command to KServer\n");
        return -1;
    }
    
    return 0;
}

static int kclient_get_devices(struct kclient *kcl)
{
    int line_cnt = 0;
    int i, tmp_buff_cnt;
    int bytes_read = 0;
    int dev_cnt = 0;
    
    bool is_dev_id = 0, is_dev_name = 0;
    
    struct operation tmp_op;
    
    char tmp_buff[2048];
    
    const char cmd[] = "1|1|\n";
    struct rcv_buff rcv_buffer;
    
    if (kclient_send_string(kcl, cmd) < 0)
        return -1;
    
    bytes_read = kclient_rcv_esc_seq(kcl, &rcv_buffer, "EOC");
    
    if (bytes_read < 0) {
        return -1;
    }
    
    char *buffer = rcv_buffer.buffer;
    
    // Parse rcv_buffer
    tmp_buff[0] = '\0';
    tmp_buff_cnt = 0;
    kcl->devices[dev_cnt].ops_num = 0;
    
    for (i=0; i<bytes_read; i++) {
        if (line_cnt == 0) {
            if (buffer[i] != '\n') {
                tmp_buff[tmp_buff_cnt] = buffer[i];
                tmp_buff_cnt++;
            } else {
                tmp_buff[tmp_buff_cnt] = '\0';
                kcl->max_num_op = (int) strtol(tmp_buff, (char **)NULL, 10);
                
                if (kcl->max_num_op > MAX_OP_NUM) {
                    DEBUG_MSG("MAX_OP_NUM overflow\n");
                    return -1;
                }
                              
                tmp_buff[0] = '\0';
                tmp_buff_cnt = 0;
                line_cnt++;
                is_dev_id = 1;
            }
        } else {
            if (buffer[i] == ':') {
                if (is_dev_id) {
                    tmp_buff[tmp_buff_cnt] = '\0';
                    kcl->devices[dev_cnt].id 
                        = (int) strtol(tmp_buff, (char **)NULL, 10);
                    tmp_buff[0] = '\0';
                    tmp_buff_cnt = 0;
                    is_dev_id = 0;
                    is_dev_name = 1;
                } else if (is_dev_name) {
                    tmp_buff[tmp_buff_cnt] = '\0';
                    strcpy(kcl->devices[dev_cnt].name, tmp_buff);
                    tmp_buff[0] = '\0';
                    tmp_buff_cnt = 0;
                    is_dev_id = 0;
                    is_dev_name = 0;
                } else { // is_op_name
                    if (tmp_buff_cnt != 0) {
                        tmp_buff[tmp_buff_cnt] = '\0';
                        strcpy(tmp_op.name, tmp_buff);
                        append_op_to_dev(&(kcl->devices[dev_cnt]), &tmp_op);
                        tmp_buff[0] = '\0';
                        tmp_buff_cnt = 0;
                    }
                }
            } else if (buffer[i] == '\n') {
                if (tmp_buff_cnt != 0) {
                    tmp_buff[tmp_buff_cnt] = '\0';
                    strcpy(tmp_op.name, tmp_buff);
                    append_op_to_dev(&(kcl->devices[dev_cnt]), &tmp_op);
                    tmp_buff[0] = '\0';
                    tmp_buff_cnt = 0;
                }
                
                if (dev_cnt+1 == kcl->devs_num) {
                    break;
                }
                
                dev_cnt++;
                kcl->devices[dev_cnt].ops_num = 0;
                is_dev_id = 1;
                line_cnt++;                
            } else {
                tmp_buff[tmp_buff_cnt] = buffer[i];
                tmp_buff_cnt++;
            }
        }
    }
    
#if VERBOSE
    debug_display_kclient(kcl);
#endif
    
    return 0;
}

/* 
 *  --------- Kill session ---------
 */

int kill_session(struct kclient *kcl, int sess_id)
{
    char cmd[64];
    snprintf(cmd, 64, "1|5|%i|\n", sess_id);
    
    if (kclient_send_string(kcl, cmd) < 0)
        return -1;

    return 0;
}

/*
 * External functions
 */
 
#if defined (__linux__)
static int open_kclient_tcp_socket(struct kclient *kcl)
{
    int sockfd;
    
    kcl->sockfd = -1;
  
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        fprintf(stderr, "Can't open socket\n");		
        return -1;
    }

    kcl->sockfd = sockfd;
    return 0;
}
#elif defined (__MINGW32__)
static int open_kclient_tcp_socket(struct kclient *kcl)
{
    WSADATA WsaDat;
    SOCKET Socket;

    kcl->sockfd = INVALID_SOCKET;

    if (WSAStartup(MAKEWORD(2,2), &WsaDat) != 0) {
        fprintf(stderr, "Winsock initialization failed\n");
        WSACleanup();
        return -1;
    }

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (Socket == INVALID_SOCKET) {
        fprintf(stderr, "Can't open socket\n");
        WSACleanup();
        return  -1;
    }

    kcl->sockfd = Socket;
    return 0;
}
#endif

#if defined (__linux__)
static int open_kclient_unix_socket(struct kclient *kcl)
{
    int unix_sockfd;

    kcl->unix_sockfd = -1;

    unix_sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (unix_sockfd < 0) {
        fprintf(stderr, "Can't open Unix socket\n");		
        return -1;
    }

    kcl->unix_sockfd = unix_sockfd;
    return 0;
}
#endif // (__linux__)

static void set_serveraddr(struct kclient *kcl, char *host, int port)
{    
    memset(&(kcl->serveraddr), 0, sizeof(kcl->serveraddr));
    (kcl->serveraddr).sin_family = AF_INET;
    (kcl->serveraddr).sin_addr.s_addr = inet_addr(host);
    (kcl->serveraddr).sin_port = htons(port);
}

#if defined (__linux__)
static void set_unixserveraddr(struct kclient *kcl, const char *sock_path)
{    
    (kcl->unixserveraddr).sun_family = AF_UNIX;
    strcpy((kcl->unixserveraddr).sun_path, sock_path);
}

static int unix_socket_connect(struct kclient *kcl)
{
    int len = strlen((kcl->unixserveraddr).sun_path) 
                    + sizeof((kcl->unixserveraddr).sun_family);

    if (connect(kcl->unix_sockfd, 
                (struct sockaddr *)&kcl->unixserveraddr, len) < 0) {
        fprintf(stderr, "Can't connect to KServer\n");
        close(kcl->unix_sockfd);
        return -1;
    }

    return 0;
}
#endif // (__linux__)

#if defined (__linux__)
static int set_kclient_sock_options(struct kclient *kcl)
{
    int on = 1;

    if (connect(kcl->sockfd, (struct sockaddr*) &(kcl->serveraddr), 
                sizeof(kcl->serveraddr)) < 0) {
        fprintf(stderr, "Can't connect to KServer\n");
        close(kcl->sockfd);
        return -1;
    }

    if (setsockopt(kcl->sockfd, IPPROTO_TCP, TCP_NODELAY, 
                   (const char *)&on, sizeof(int)) < 0) {
        fprintf(stderr, "Can't set TCP_NODELAY option\n");
        close(kcl->sockfd);
        return -1;
    }

    return 0;
}
#elif defined (__MINGW32__)
static int set_kclient_sock_options(struct kclient *kcl)
{
    int on = 1;

    if (connect(kcl->sockfd, (SOCKADDR*)(&(kcl->serveraddr)), 
                sizeof(kcl->serveraddr)) != 0) {
        fprintf(stderr, "Can't connect to KServer\n");
        WSACleanup();
        return -1;
    }

    if (setsockopt(kcl->sockfd, IPPROTO_TCP, TCP_NODELAY, 
                   (const char *)&on, sizeof(int)) < 0) {
        fprintf(stderr, "Can't set TCP_NODELAY option\n");
        WSACleanup();
        return -1;
    }

    return 0;
}
#endif

KOHERON_LIB_EXPORT
struct kclient* kclient_connect(char *host, int port)
{  
    struct kclient *kcl = malloc(sizeof *kcl);

    if (kcl == NULL) {
        fprintf(stderr, "Can't allocate Kclient memory\n");
        return NULL;
    }

    kcl->conn_type = TCP;
    kcl->unix_sockfd = -1;

    if (open_kclient_tcp_socket(kcl) < 0)
        return NULL;

    set_serveraddr(kcl, host, port);

    if (set_kclient_sock_options(kcl) < 0)
        return NULL;

    if (kclient_get_devices(kcl) < 0)
        return NULL;

    return kcl;
}

#if defined (__linux__)
KOHERON_LIB_EXPORT
struct kclient* kclient_unix_connect(const char *sock_path)
{
    struct kclient *kcl = malloc(sizeof *kcl);

    if (kcl == NULL) {
        fprintf(stderr, "Can't allocate Kclient memory\n");
        return NULL;
    }

    kcl->conn_type = UNIX;
    kcl->sockfd = -1;

    if (open_kclient_unix_socket(kcl) < 0)
        return NULL;

    set_unixserveraddr(kcl, sock_path);

    if (unix_socket_connect(kcl) < 0) 
        return NULL;

    if (kclient_get_devices(kcl) < 0)
        return NULL;

    return kcl;
}
#endif // (__linux__)

#if defined (__linux__)
static void close_kclient_socket(struct kclient *kcl)
{
    if (kcl->conn_type == TCP && kcl->sockfd >= 0) {
        close(kcl->sockfd);
    }
    
    if (kcl->conn_type == UNIX && kcl->unix_sockfd >= 0) {
        close(kcl->unix_sockfd);
    }
}
#elif defined (__MINGW32__)
static void close_kclient_socket(struct kclient *kcl)
{
    if (kcl->sockfd != INVALID_SOCKET) {
        shutdown(kcl->sockfd, SD_SEND);
        closesocket(kcl->sockfd);
        WSACleanup();
    }
}
#endif

KOHERON_LIB_EXPORT
void kclient_shutdown(struct kclient *kcl)
{
    close_kclient_socket(kcl);
    
    if (kcl != NULL) {
        free(kcl);
        kcl = NULL;
    }
}

