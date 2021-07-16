#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define STR_FIXED       "TVLA set Fixed"
#define STR_RAND        "TVLA set Random"

#define TFM_DATA(tfm)   ((struct tfm_split_tvla *) (tfm)->data)

struct tfm_split_tvla
{
    bool which;
};

int __tfm_split_tvla_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_split_tvla_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_split_tvla_trace_size(struct trace_set *ts)
{
    // about half the time, we won't actually be storing samples
    return ts->title_size + ts->data_size +
            (ts->num_samples / 2) * sizeof(float);
}

void __tfm_split_tvla_exit(struct trace_set *ts)
{}

int __get_trace_type(struct trace *t, bool *type)
{
    if(t->title)
    {
        if(strncmp(t->title, STR_FIXED, strlen(STR_FIXED)) == 0)
        {
            *type = TVLA_FIXED;
            return 0;
        }

        if(strncmp(t->title, STR_RAND, strlen(STR_RAND)) == 0)
        {
            *type = TVLA_RANDOM;
            return 0;
        }

        err("Invalid trace title, not a TVLA dataset?\n");
        return -EINVAL;
    }
    else return 0;
}

int __tfm_split_tvla_get(struct trace *t)
{
    int ret;
    bool type;

    struct trace *prev_trace;
    struct tfm_split_tvla *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to get trace from previous set\n");
        return ret;
    }

    ret = __get_trace_type(prev_trace, &type);
    if(ret < 0)
    {
        err("Failed to get trace type from title\n");
        goto __done;
    }

    if(type == tfm->which)
        ret = passthrough(t);
    else
    {
        t->title = NULL;
        t->data = NULL;
        t->samples = NULL;
        ret = 0;
    }

__done:
    trace_free(prev_trace);
    return ret;
}

void __tfm_split_tvla_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_split_tvla(struct tfm **tfm, bool which)
{
    struct tfm *res;

    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_split_tvla);

    res->data = calloc(1, sizeof(struct tfm_split_tvla));
    if(!res->data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->which = which;
    *tfm = res;
    return 0;
}