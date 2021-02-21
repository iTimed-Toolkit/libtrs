#include "transform.h"
#include "libtrs.h"
#include "../lib/__libtrs_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define STR_FIXED       "TVLA set Fixed"
#define STR_RAND        "TVLA set Random"

struct tfm_split_tvla
{
    struct tfm_generic gen;
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

void __tfm_split_tvla_exit(struct trace_set *ts)
{

}

int __get_trace_type(struct trace *t, bool *type)
{
    int ret;
    char *title;

    ret = trace_title(t, &title);
    if(ret < 0)
        return ret;

    if(title)
    {
        if(strncmp(title, STR_FIXED, strlen(STR_FIXED)) == 0)
        {
            *type = true;
            return 0;
        }

        if(strncmp(title, STR_RAND, strlen(STR_RAND)) == 0)
        {
            *type = false;
            return 0;
        }

        return -EINVAL;
    }
    else return 0;
}


int __tfm_split_tvla_title(struct trace *t, char **title)
{
    int ret;
    bool type;
    char *prev_title, *res;

    struct trace *prev_trace;
    struct tfm_split_tvla *tfm = t->owner->tfm;

    ret = trace_get(t->owner->prev, &prev_trace, t->start_offset, false);
    if(ret < 0)
        return ret;

    ret = __get_trace_type(prev_trace, &type);
    if(ret < 0)
        goto __out;

    if(type == tfm->which)
    {
        ret = trace_title(prev_trace, &prev_title);
        if(ret < 0)
            goto __out;

        if(prev_title)
        {
            res = calloc(t->owner->title_size, sizeof(char));
            if(res < 0)
            {
                ret = -ENOMEM;
                goto __out;
            }

            memcpy(res, prev_title, t->owner->title_size);
            *title = res;
        }
        else
        {
            *title = NULL;
            ret = 0;
        }
    }
    else
    {
        *title = NULL;
        ret = 0;
    }

__out:
    trace_free(prev_trace);
    return ret;
}

int __tfm_split_tvla_data(struct trace *t, uint8_t **data)
{
    int ret;
    bool type;
    uint8_t *prev_data, *res;

    struct trace *prev_trace;
    struct tfm_split_tvla *tfm = t->owner->tfm;

    ret = trace_get(t->owner->prev, &prev_trace, t->start_offset, false);
    if(ret < 0)
        return ret;

    ret = __get_trace_type(prev_trace, &type);
    if(ret < 0)
        goto __out;

    if(type == tfm->which)
    {
        ret = trace_data_all(prev_trace, &prev_data);
        if(ret < 0)
            goto __out;

        if(prev_data)
        {
            res = calloc(t->owner->data_size, sizeof(uint8_t));
            if(res < 0)
            {
                ret = -ENOMEM;
                goto __out;
            }

            memcpy(res, prev_data, t->owner->data_size);
            *data = res;
        }
        else
        {
            *data = NULL;
            ret = 0;
        }
    }
    else
    {
        *data = NULL;
        ret = 0;
    }

__out:
    trace_free(prev_trace);
    return ret;
}

int __tfm_split_tvla_samples(struct trace *t, float **samples)
{
    int ret;
    bool type;
    float *prev_samples, *res;

    struct trace *prev_trace;
    struct tfm_split_tvla *tfm = t->owner->tfm;

    ret = trace_get(t->owner->prev, &prev_trace, t->start_offset, false);
    if(ret < 0)
        return ret;

    ret = __get_trace_type(prev_trace, &type);
    if(ret < 0)
        goto __out;

    if(type == tfm->which)
    {
        ret = trace_samples(prev_trace, &prev_samples);
        if(ret < 0)
            goto __out;

        if(prev_samples)
        {
            res = calloc(t->owner->datatype & 0xF, t->owner->num_samples);
            if(res < 0)
            {
                ret = -ENOMEM;
                goto __out;
            }

            memcpy(res, prev_samples, (t->owner->datatype & 0xF) *
                                      t->owner->num_samples);
            *samples = res;
        }
        else
        {
            *samples = NULL;
            ret = 0;
        }
    }
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
    free(t->buffered_title);
}

void __tfm_split_tvla_free_data(struct trace *t)
{
    free(t->buffered_data);
}

void __tfm_split_tvla_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_split_tvla(void **tfm, bool which)
{
    struct tfm_split_tvla *res;

    res = calloc(1, sizeof(struct tfm_split_tvla));
    if(!res)
        return -ENOMEM;

    ASSIGN_TFM_FUNCS(res, __tfm_split_tvla);
    res->which = which;

    *tfm = res;
    return 0;
}