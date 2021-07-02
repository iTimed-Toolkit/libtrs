#include "trace.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <openssl/aes.h>
#include <openssl/evp.h>

#define NET_ARG(ts)     ((struct backend_net_arg *) (ts)->backend->arg)

struct backend_net_arg
{
    char *serv_ip;
    int serv_port;
    struct sockaddr_in serv_addr;
};

int __net_cmd(FILE *netfile, bknd_net_cmd_t cmd,
              void *resp, size_t resp_len)
{
    size_t written, read;
    written = fwrite(&cmd, sizeof(bknd_net_cmd_t), 1, netfile);
    if(written != 1)
    {
        err("Failed to send command to server\n");
        return -errno;
    }

    read = fread(resp, resp_len, 1, netfile);
    if(read != 1)
    {
        err("Failed to read response from server\n");
        return -errno;
    }

    return 0;
}

int __open_connection(struct backend_net_arg *arg, FILE **res)
{
    int sockfd, ret;
    FILE *netfile;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0)
    {
        err("Failed to create socket: %s\n", strerror(errno));
        return -errno;
    }

    ret = connect(sockfd, (struct sockaddr *) &arg->serv_addr, sizeof(arg->serv_addr));
    if(ret < 0)
    {
        err("Failed to connect socket: %s\n", strerror(errno));
        ret = -errno;
        goto __close_socket;
    }

    netfile = fdopen(sockfd, "rw+");
    if(!netfile)
    {
        err("Failed to open a FILE* from sockfd: %s\n", strerror(errno));
        ret = -errno;
        goto __close_socket;
    }

    *res = netfile;
    return 0;

__close_socket:
    close(sockfd);
    return ret;
}

void __close_connection(FILE *netfile)
{
    bknd_net_cmd_t cmd = NET_CMD_DIE;
    int ret = send_over_socket(&cmd, sizeof(bknd_net_cmd_t), netfile);
    if(ret < 0)
        err("Failed to send die command to server\n");

    fclose(netfile);
}

int backend_net_open(struct trace_set *ts)
{
    int ret;
    bknd_net_cmd_t cmd = NET_CMD_INIT;
    struct bknd_net_init init;
    FILE *netfile;

    ret = __open_connection(NET_ARG(ts), &netfile);
    if(!netfile)
    {
        err("Failed to open new connection\n");
        return ret;
    }

    // then, initialize the trace set
    ret = send_over_socket(&cmd, sizeof(bknd_net_cmd_t), netfile);
    if(ret < 0)
    {
        err("Failed to send command to server\n");
        goto __close_connection;
    }

    ret = recv_over_socket(&init, sizeof(struct bknd_net_init), netfile);
    if(ret < 0)
    {
        err("Failed to receive init struct from server\n");
        goto __close_connection;
    }

    __close_connection(netfile);
    ts->num_traces = init.num_traces;
    ts->num_samples = init.num_samples;
    ts->datatype = init.datatype;
    ts->title_size = init.title_size;
    ts->data_size = init.data_size;
    ts->yscale = init.yscale;
    return 0;

__close_connection:
    __close_connection(netfile);
    return ret;
}

int backend_net_create(struct trace_set *ts)
{
    err("Creating a network backend is invalid -- needs to be opened\n");
    return -EINVAL;
}

int backend_net_close(struct trace_set *ts)
{
    free(NET_ARG(ts));
    return 0;
}

int backend_net_read(struct trace *t)
{
    int ret;
    FILE *netfile;
    bknd_net_cmd_t cmd = NET_CMD_GET;

    size_t len;
    uint8_t *buf;

    len = t->owner->title_size + t->owner->data_size +
          t->owner->num_samples * sizeof(float);
    buf = calloc(len, 1);
    if(!buf)
    {
        err("Failed to allocate receiving buf for trace\n");
        return -ENOMEM;
    }

    ret = __open_connection(NET_ARG(t->owner), &netfile);
    if(ret < 0)
    {
        err("Failed to open new connection\n");
        goto __free_buf;
    }

    ret = send_over_socket(&cmd, sizeof(bknd_net_cmd_t), netfile);
    if(ret >= 0)
        ret = send_over_socket(&TRACE_IDX(t), sizeof(size_t), netfile);
    if(ret >= 0)
        ret = recv_over_socket(buf, len, netfile);

    __close_connection(netfile);
    if(ret < 0)
    {
        err("Protocol error\n");
        goto __free_buf;
    }

    t->title = calloc(t->owner->title_size, 1);
    if(t->title)
        t->data = calloc(t->owner->data_size, 1);
    if(t->data)
        t->samples = calloc(t->owner->num_samples, sizeof(float));
    if(!t->data)
    {
        err("Failed to allocate some trace data\n");
        goto __free_trace;
    }

    memcpy(t->title, &buf[0], t->owner->title_size);
    memcpy(t->data, &buf[t->owner->title_size], t->owner->data_size);
    memcpy(t->samples, &buf[t->owner->title_size + t->owner->data_size],
           t->owner->num_samples * sizeof(float));

    free(buf);
    return 0;

__free_trace:
    if(t->title)
        free(t->title);

    if(t->data)
        free(t->data);

    if(t->samples)
        free(t->samples);

__free_buf:
    free(buf);
    return ret;
}

int backend_net_write(struct trace *t)
{
    err("Writing to a network backend is invalid\n");
    return -EINVAL;
}

int create_backend_net(struct trace_set *ts, const char *name)
{
    int ret;
    char *tok, **curr = (char **) &name;

    struct backend_net_arg *arg;
    struct hostent *server;

    struct backend_intf *res = calloc(1, sizeof(struct backend_intf));
    if(!res)
    {
        err("Failed to allocate backend interface struct\n");
        return -ENOMEM;
    }

    res->open = backend_net_open;
    res->create = backend_net_create;
    res->close = backend_net_close;
    res->read = backend_net_read;
    res->write = backend_net_write;

    arg = calloc(1, sizeof(struct backend_net_arg));
    if(!arg)
    {
        err("Failed to allocate argument for backend\n");
        ret = -ENOMEM;
        goto __free_res;
    }

    tok = strsep(curr, " ");
    arg->serv_ip = calloc(strlen(tok) + 1, sizeof(char));
    if(!arg->serv_ip)
    {
        err("Failed to allocate memory for IP address\n");
        ret = -ENOMEM;
        goto __free_arg;
    }

    strcpy(arg->serv_ip, tok);
    arg->serv_port = (int) strtol(*curr, NULL, 10);
    server = gethostbyname(arg->serv_ip);
    if(!server)
    {
        err("Failed to look up server name\n");
        ret = -h_errno;
        goto __free_str;
    }

    arg->serv_addr.sin_family = AF_INET;
    arg->serv_addr.sin_port = htons(arg->serv_port);
    memcpy(&arg->serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    res->arg = arg;
    ts->backend = res;
    return 0;

__free_str:
    free(arg->serv_ip);

__free_arg:
    free(arg);

__free_res:
    free(res);
    return ret;
}