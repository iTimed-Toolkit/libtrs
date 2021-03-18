#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>

int __tfm_nop_init(struct trace_set *ts)
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

size_t __tfm_nop_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __tfm_nop_title(struct trace *t, char **title)
{
    return passthrough_title(t, title);
}

int __tfm_nop_data(struct trace *t, uint8_t **data)
{
    return passthrough_data(t, data);
}

int __tfm_nop_samples(struct trace *t, float **samples)
{
    return passthrough_samples(t, samples);
}

void __tfm_nop_exit(struct trace_set *ts){}

void __tfm_nop_free_title(struct trace *t)
{
    passthrough_free_title(t);
}

void __tfm_nop_free_data(struct trace *t)
{
    passthrough_free_data(t);
}

void __tfm_nop_free_samples(struct trace *t)
{
    passthrough_free_samples(t);
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
    res->tfm_data = NULL;

    *tfm = res;
    return 0;
}