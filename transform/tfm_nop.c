#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>

int __tfm_nop_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_nop_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_nop_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_nop_exit(struct trace_set *ts){}



int __tfm_nop_get(struct trace *t)
{
    return passthrough_all(t);
}

void __tfm_nop_free(struct trace *t)
{
    passthrough_free_all(t);
}


int tfm_nop(struct tfm **tfm)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_nop);
    res->data = NULL;

    *tfm = res;
    return 0;
}