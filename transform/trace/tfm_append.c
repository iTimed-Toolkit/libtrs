#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <string.h>

int __tfm_append_init(struct trace_set *ts)
{
    int ret;
    char *path = ts->tfm->data;
    struct trace_set *append;

    ret = ts_open(&append, path);
    if(ret < 0)
    {
        err("Failed to open trace set to append\n");
        return ret;
    }

    if((ts_num_samples(append) != ts->prev->num_samples) ||
       (append->title_size != ts->prev->title_size) ||
       (append->data_size) != ts->prev->data_size)
    {
        err("Incompatible trace sets: mismatch in sizes\n");

        ts_close(append);
        return -EINVAL;
    }

    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces + ts_num_traces(append);

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = append->datatype;
    ts->yscale = append->yscale;

    ts->tfm_state = append;
    return 0;
}

int __tfm_append_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_append_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_append_exit(struct trace_set *ts)
{
    ts_close(ts->tfm_state);
    free(ts->prev->tfm->data);
}

int __tfm_append_get(struct trace *t)
{
    int ret;
    size_t index;

    struct trace_set *append;
    struct trace *prev_trace;

    if(TRACE_IDX(t) < ts_num_traces(t->owner->prev))
        return passthrough_all(t);
    else
    {
        index = TRACE_IDX(t) - ts_num_traces(t->owner->prev);
        append = t->owner->tfm_state;

        ret = trace_get(append, &prev_trace, index);
        if(ret < 0)
        {
            err("Failed to get trace from previous trace set\n");
            return ret;
        }

        ret = copy_title(t, prev_trace);
        if(ret >= 0)
            ret = copy_data(t, prev_trace);
        if(ret >= 0)
            ret = copy_samples(t, prev_trace);

        if(ret < 0)
        {
            err("Failed to copy something\n");
            passthrough_free_all(t);
        }

        trace_free(prev_trace);
        return ret;
    }
}

void __tfm_append_free(struct trace *t)
{
    passthrough_free_all(t);
}

int tfm_append(struct tfm **tfm, const char *path)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_append);

    res->data = calloc(strlen(path) + 1, sizeof(char));
    if(!res->data)
    {
        err("Failed to allocate memory for path name\n");
        free(res);
        return -ENOMEM;
    }

    strcpy(res->data, path);

    *tfm = res;
    return 0;
}