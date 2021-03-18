#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define STR_FIXED       "TVLA set Fixed"
#define STR_RAND        "TVLA set Random"

#define TFM_DATA(tfm)   ((struct tfm_split_tvla *) (tfm)->tfm_data)

struct tfm_split_tvla
{
    bool which;
};

int __tfm_split_tvla_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->input_offs = ts->prev->input_offs;
    ts->input_len = ts->prev->input_len;
    ts->output_offs = ts->prev->output_offs;
    ts->output_len = ts->prev->output_len;
    ts->key_offs = ts->prev->key_offs;
    ts->key_len = ts->prev->key_len;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

size_t __tfm_split_tvla_trace_size(struct trace_set *ts)
{
    // about half the time, we won't actually be storing samples
    return ts->title_size + ts->data_size +
            (ts->num_samples / 2) * sizeof(float);
}

void __tfm_split_tvla_exit(struct trace_set *ts)
{

}

int __get_trace_type(struct trace *t, bool *type)
{
    int ret;
    char *title;

    ret = trace_title(t, &title);
    if(ret < 0)
    {
        err("Failed to get title from trace\n");
        return ret;
    }

    if(title)
    {
        if(strncmp(title, STR_FIXED, strlen(STR_FIXED)) == 0)
        {
            *type = TVLA_FIXED;
            return 0;
        }

        if(strncmp(title, STR_RAND, strlen(STR_RAND)) == 0)
        {
            *type = TVLA_RANDOM;
            return 0;
        }

        err("Invalid trace title, not a TVLA dataset?\n");
        return -EINVAL;
    }
    else return 0;
}


int __tfm_split_tvla_title(struct trace *t, char **title)
{
    return passthrough_title(t, title);
}

int __tfm_split_tvla_data(struct trace *t, uint8_t **data)
{
    return passthrough_data(t, data);
}

int __tfm_split_tvla_samples(struct trace *t, float **samples)
{
    int ret;
    bool type;

    struct trace *prev_trace;
    struct tfm_split_tvla *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t), false);
    if(ret < 0)
    {
        err("Failed to get trace from previous set\n");
        return ret;
    }

    ret = __get_trace_type(prev_trace, &type);
    if(ret < 0)
    {
        err("Failed to get trace type from title\n");
        goto __out;
    }

    if(type == tfm->which)
        ret = passthrough_samples(t, samples);
    else
    {
        *samples = NULL;
        ret = 0;
    }

__out:
    trace_free(prev_trace);
    return ret;
}

void __tfm_split_tvla_free_title(struct trace *t)
{
    passthrough_free_title(t);
}

void __tfm_split_tvla_free_data(struct trace *t)
{
    passthrough_free_data(t);
}

void __tfm_split_tvla_free_samples(struct trace *t)
{
    passthrough_free_samples(t);
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

    res->tfm_data = calloc(1, sizeof(struct tfm_split_tvla));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->which = which;
    *tfm = res;
    return 0;
}