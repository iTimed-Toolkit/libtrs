#include "trace.h"
#include "__trace_internal.h"

#include "platform.h"
#include "net_types.h"

#include <unistd.h>
#include <stdlib.h>

#include "list.h"

struct export_thread_arg
{
    struct list_head list;
    LT_THREAD_TYPE handle;
    struct trace_set *ts;

    bool running;
    int retval;
    int cli_sockfd;
};

LT_THREAD_FUNC(__ts_export_thread, thread_arg)
{
    int ret;
    size_t index, trace_len;

    bknd_net_cmd_t cmd;
    struct bknd_net_init init;
    struct export_thread_arg *arg = thread_arg;

    struct trace *t;
    uint8_t *trace_buf = NULL;

    trace_len = arg->ts->title_size + arg->ts->data_size +
                arg->ts->num_samples * sizeof(float);
    while(1)
    {
        ret = p_safesocket_read(arg->cli_sockfd, &cmd, sizeof(bknd_net_cmd_t));
        if(ret < 0)
        {
            err("Failed to read command from client\n");
            goto __done;
        }

        switch(cmd)
        {
            case NET_CMD_INIT:
                init.num_traces = arg->ts->num_traces;
                init.num_samples = arg->ts->num_samples;
                init.datatype = arg->ts->datatype;
                init.title_size = arg->ts->title_size;
                init.data_size = arg->ts->data_size;
                init.yscale = arg->ts->yscale;

                ret = p_safesocket_write(arg->cli_sockfd, &init, sizeof(struct bknd_net_init));
                if(ret < 0)
                {
                    err("Failed to send init struct to client\n");
                    goto __done;
                }
                break;

            case NET_CMD_GET:
                ret = p_safesocket_read(arg->cli_sockfd, &index, sizeof(size_t));
                if(ret < 0)
                {
                    err("Failed to get index of requested trace\n");
                    goto __done;
                }

                ret = trace_get(arg->ts, &t, index);
                if(ret < 0)
                {
                    err("Failed to get requested trace\n");
                    goto __done;
                }

                if(!trace_buf)
                {
                    trace_buf = calloc(trace_len, 1);
                    if(!trace_buf)
                    {
                        err("Failed to allocaate trace buffer\n");
                        goto __free_trace;
                    }
                }

                memcpy(&trace_buf[0], t->title, t->owner->title_size);
                memcpy(&trace_buf[t->owner->title_size], t->data, t->owner->data_size);
                memcpy(&trace_buf[t->owner->title_size + t->owner->data_size],
                       t->samples, t->owner->num_samples * sizeof(float));

                trace_free(t);
                ret = p_safesocket_write(arg->cli_sockfd, trace_buf, trace_len);
                if(ret < 0)
                {
                    err("Failed to send trace data over socket\n");
                    goto __done;
                }
                break;

            case NET_CMD_DIE:
                ret = 0; goto __done;

            default:
                err("Unrecognized command\n");
                ret = -EINVAL; goto __done;
        }
    }

__free_trace:
    trace_free(t);

__done:
    if(trace_buf)
        free(trace_buf);

    close(arg->cli_sockfd);
    arg->retval = ret;
    arg->running = false;
    return NULL;
}

struct export
{
    LT_THREAD_TYPE handle;
    int ret, port;

    struct trace_set *ts;
    struct list_head threads;
};

LT_THREAD_FUNC(__ts_export_controller, controller_arg)
{
    int ret;
    struct export *arg = controller_arg;
    struct export_thread_arg *entry, *n;

    LT_SOCK_TYPE serv_sockfd, cli_sockfd;
    ret = p_socket_server(arg->port, &serv_sockfd);
    if(ret < 0)
    {
        err("Failed to create a server socket\n");
        arg->ret = ret;
        return NULL;
    }

    while(1)
    {
        ret = p_socket_accept(serv_sockfd, &cli_sockfd);
        if(ret < 0)
        {
            err("Failed to accept a new client\n");
            goto __close_socket;
        }

        entry = calloc(1, sizeof(struct export_thread_arg));
        if(!entry)
        {
            err("Failed to allocate export thread entry\n");
            close(cli_sockfd);
            goto __close_socket;
        }

        LIST_HEAD_INIT_INLINE(entry->list);
        entry->ts = arg->ts;
        entry->running = true;
        entry->retval = 0;
        entry->cli_sockfd = cli_sockfd;

        ret = p_thread_create(&entry->handle, __ts_export_thread, entry);
        if(ret < 0)
        {
            err("Failed to create new thread\n");
            free(entry);
            close(cli_sockfd);
            goto __close_socket;
        }

        list_add_tail(&entry->list, &arg->threads);
        list_for_each_entry_safe(entry, n, &arg->threads, list)
        {
            if(!entry->running)
            {
                p_thread_join(entry->handle);
                list_del(&entry->list);

                if(entry->retval < 0)
                {
                    err("Export thread encountered error\n");
                    goto __close_socket;
                }

                free(entry);
            }
        }
    }

__close_socket:
    p_socket_close(serv_sockfd);
    arg->ret = ret;
    return NULL;
}

int ts_export(struct trace_set *ts, int port)
{
    struct export arg = {
        .ret = 0,
        .port = port,
        .ts = ts,
        .threads = LIST_HEAD_INIT(arg.threads)
    };

    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    __ts_export_controller(&arg);
    return arg.ret;
}

int ts_export_async(struct trace_set *ts, int port, struct export **export)
{
    int ret;
    struct export *res;

    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct export));
    if(!res)
    {
        err("Failed to allocate export struct\n");
        return -ENOMEM;
    }

    res->ret = 0;
    res->ts = ts;
    res->port = port;

    ret = p_thread_create(&res->handle, __ts_export_controller, res);
    if(ret < 0)
    {
        err("Failed to create controller pthread\n");
        free(res);
        return -EINVAL;
    }

    *export = res;
    return 0;
}

int ts_export_join(struct export *export)
{
    int ret;
    if(!export)
    {
        err("Invalid export struct\n");
        return -EINVAL;
    }

    p_thread_join(export->handle);
    ret = export->ret;

    free(export);
    return ret;
}