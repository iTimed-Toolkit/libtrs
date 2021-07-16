#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "crypto.h"

#include <errno.h>

int __tfm_verify_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_verify_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_verify_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_verify_exit(struct trace_set *ts){}

int __verify_generic(struct trace *t, crypto_t which, bool *result)
{
    int ret;
    struct trace *prev_trace;

    ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to get trace from previous trace set\n");
        return ret;
    }

    if(prev_trace->data)
    {
        switch(which)
        {
            case AES128:
                *result = verify_aes128(prev_trace->data);
                ret = 0;
                break;

            default:
                err("Unrecognized verification model\n");
                ret = -EINVAL;
                goto __done;
        }
    }
    else
    {
        // not an error condition necessarily
        debug("No data to verify\n");
        *result = false;
        return 0;
    }

__done:
    trace_free(prev_trace);
    return ret;
}

int __tfm_verify_get(struct trace *t)
{
    int ret;
    crypto_t which;
    bool verified = false;

    which = (crypto_t) t->owner->tfm->data;
    ret = __verify_generic(t, which, &verified);
    if(ret < 0)
    {
        err("Error in verification\n");
        return ret;
    }

    if(verified)
        return passthrough(t);
    else
    {
        debug("Trace %li failed verification\n", TRACE_IDX(t));
        t->title = NULL;
        t->data = NULL;
        t->samples = NULL;
        return 0;
    }
}

void __tfm_verify_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_verify(struct tfm **tfm, crypto_t which)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_verify);
    res->data = (void *) which;

    *tfm = res;
    return 0;
}