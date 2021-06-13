#include "transform.h"

#include "list.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#define TFM_DATA(tfm)   ((struct tfm_wait_on *) (tfm)->data)

struct tfm_wait_on
{
    port_t port;
    size_t bufsize;
};

struct __request_entry
{
    struct list_head list;
    int index;
    sem_t *signal;
};

struct __waiter_entry
{
    struct list_head list;
    sem_t lock;

    port_t port;
    struct trace_set *set;

    size_t ntraces;
    struct trace_cache *available;
    struct list_head traces_wanted;
    struct list_head traces_blocked;
};

int __tfm_wait_on_push(void *arg, port_t port, int nargs, ...)
{
    int ret;
    va_list arg_list;

    int index;
    char *pushed_title;
    uint8_t *pushed_data;
    float *pushed_samples;

    struct list_head *queue = arg;
    struct __waiter_entry *curr_waiter;
    struct trace *new_trace;
    struct __request_entry *curr_req, *n_req;

    if(nargs != 4)
    {
        err("Invalid argument count\n");
        return -EINVAL;
    }

    va_start(arg_list, nargs);
    index = va_arg(arg_list, int);
    pushed_title = va_arg(arg_list, char *);
    pushed_data = va_arg(arg_list, uint8_t *);
    pushed_samples = va_arg(arg_list, float *);
    va_end(arg_list);

    debug("got push for port %i, index %i\n", port, index);
    list_for_each_entry(curr_waiter, queue, list)
    {
        if(curr_waiter->port == port)
        {
            new_trace = calloc(1, sizeof(struct trace));
            if(!new_trace)
            {
                err("Failed to allocate for new trace\n");
                return -ENOMEM;
            }

            if(pushed_title)
            {
                new_trace->title = calloc(curr_waiter->set->title_size, sizeof(char));
                if(!new_trace->title)
                {
                    err("Failed to allocate entry title\n");
                    goto __free_trace;
                }

                memcpy(new_trace->title, pushed_title,
                       curr_waiter->set->title_size * sizeof(char));
            }
            else new_trace->title = NULL;

            if(pushed_data)
            {
                new_trace->data = calloc(curr_waiter->set->data_size, sizeof(uint8_t));
                if(!new_trace->data)
                {
                    err("Failed to allocate entry data\n");
                    goto __free_trace;
                }

                memcpy(new_trace->data, pushed_data,
                       curr_waiter->set->data_size * sizeof(uint8_t));
            }
            else new_trace->data = NULL;

            if(pushed_samples)
            {
                new_trace->samples = calloc(curr_waiter->set->num_samples, sizeof(float));
                if(!new_trace->samples)
                {
                    err("Failed to allocate entry samples\n");
                    goto __free_trace;
                }

                memcpy(new_trace->samples, pushed_samples,
                       curr_waiter->set->num_samples * sizeof(float));
            }
            else new_trace->samples = NULL;

            new_trace->owner = curr_waiter->set;
            new_trace->start_offset = index;

            ret = sem_wait(&curr_waiter->lock);
            if(ret < 0)
            {
                err("Failed to wait on entry lock\n");
                return -errno;
            }

            ret = tc_store(curr_waiter->available, index, new_trace, false);
            if(ret < 0)
            {
                err("Failed to store new trace in cache\n");
                return ret;
            }

            // also deref to set true refcount to 0
            ret = tc_deref(curr_waiter->available, index, new_trace);
            if(ret < 0)
            {
                err("Failed to set refcount to 0\n");
                return ret;
            }

            // wake up any consumers
            list_for_each_entry_safe(curr_req, n_req, &curr_waiter->traces_wanted, list)
            {
                if(curr_req->index < index)
                {
                    if(curr_req->index < (int) (index - curr_waiter->ntraces))
                    {
                        warn("Timing out request for index %i due to push for index %i\n",
                             curr_req->index, index);

                        list_del(&curr_req->list);
                        ret = sem_post(curr_req->signal);
                        if(ret < 0)
                        {
                            err("Failed to post to consumer signal\n");
                            return -errno;
                        }
                    }

                    continue;
                }
                else if(curr_req->index > index) break;
                else
                {
                    list_del(&curr_req->list);
                    ret = sem_post(curr_req->signal);
                    if(ret < 0)
                    {
                        err("Failed to post to consumer signal\n");
                        return -errno;
                    }
                }
            }

            ret = sem_post(&curr_waiter->lock);
            if(ret < 0)
            {
                err("Failed to post to entry lock\n");
                return -errno;
            }
        }
    }

    return 0;

__free_trace:
    trace_free_memory(new_trace);
    return -ENOMEM;
}

int __tfm_wait_on_init(struct trace_set *ts)
{
    int ret, assoc;

    struct list_head *queue;
    struct __waiter_entry *entry, *curr;
    struct tfm_wait_on *tfm = TFM_DATA(ts->tfm);

    if(!ts->prev->tfm)
    {
        err("Previous trace set does not have a transformation, and therefore no ports\n");
        return -EINVAL;
    }

    ret = ts->prev->tfm->init_waiter(ts, tfm->port);
    if(ret < 0)
    {
        err("Failed to initialize waiter on previous transformation\n");
        return ret;
    }

    if(ts->prev->tfm_next)
    {
        if((void *) ts->prev->tfm_next != (void *) __tfm_wait_on_push)
        {
            err("Previous trace set already has a (different) forward transformation\n");
            return -EINVAL;
        }

        queue = ts->prev->tfm_next_arg;
    }
    else
    {
        queue = calloc(1, sizeof(struct list_head));
        if(!queue)
        {
            err("Failed to allocate waiter queue\n");
            return -ENOMEM;
        }

        LIST_HEAD_INIT_INLINE(*queue);
    }

    entry = calloc(1, sizeof(struct __waiter_entry));
    if(!entry)
    {
        err("Failed to allocate waiter data\n");
        return -ENOMEM;
    }

    if(ts_trace_size(ts) > tfm->bufsize)
    {
        err("Storage buffer size less than size of one trace\n");
        ret = -EINVAL;
        goto __free_entry;
    }

    // heuristic for associativity
    assoc = (int) log2(tfm->bufsize) - 10;
    entry->ntraces = __find_num_traces(ts, tfm->bufsize, assoc);

    ret = tc_cache_manual(&entry->available, ts->set_id, entry->ntraces / assoc, assoc);
    if(ret < 0)
    {
        err("Failed to create backing cache\n");
        goto __free_entry;
    }

    ret = sem_init(&entry->lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize entry lock\n");
        ret = -errno;
        goto __free_entry;
    }

    entry->port = tfm->port;
    entry->set = ts;
    LIST_HEAD_INIT_INLINE(entry->traces_wanted);

    list_for_each_entry(curr, queue, list)
    {
        if(curr->port > tfm->port)
            break;
    }
    list_add_tail(&entry->list, queue);

    ts->tfm_state = queue;
    ts->prev->tfm_next_arg = queue;
    ts->prev->tfm_next = (void *) __tfm_wait_on_push;
    return 0;

__free_entry:
    if(entry->available)
        tc_free(entry->available);

    free(entry);
    return ret;
}

int __tfm_wait_on_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_wait_on_trace_size(struct trace_set *ts)
{
    return ts->num_samples * sizeof(float) +
           ts->data_size + ts->title_size +
           sizeof(struct trace);
}

void __tfm_wait_on_exit(struct trace_set *ts)
{
    struct __waiter_entry *curr;
    struct tfm_wait_on *tfm = TFM_DATA(ts->tfm);
    struct list_head *queue = ts->tfm_state;

    list_for_each_entry(curr, queue, list)
    {
        if(curr->port == tfm->port && curr->set == ts)
            break;
    }

    if(curr->port == tfm->port && curr->set == ts)
    {
        // todo: wait for consumer list to drain and then free everything in buffers?
        list_del(&curr->list);
        free(curr);
    }
    else err("Unable to find registered waiter entry for given port and trace set\n");
}

int __wait_for_entry(int index, struct __waiter_entry *curr_waiter)
{
    int ret;
    struct __request_entry *request, *curr_request;
    sem_t signal;

    debug("Setting up wait request for index %i\n", index);
    ret = sem_init(&signal, 0, 0);
    if(ret < 0)
    {
        err("Failed to create consumer signal\n");
        return -errno;
    }

    request = calloc(1, sizeof(struct __request_entry));
    if(!request)
    {
        err("Failed to allocate trace index request\n");
        return -ENOMEM;
    }

    LIST_HEAD_INIT_INLINE(request->list);
    request->index = index;
    request->signal = &signal;

    list_for_each_entry(curr_request, &curr_waiter->traces_wanted, list)
    {
        if(curr_request->index > index)
            break;
    }
    list_add_tail(&request->list, &curr_request->list);

    ret = sem_post(&curr_waiter->lock);
    if(ret < 0)
    {
        err("Failed to post to queue entry lock\n");
        return -errno;
    }

    debug("Waiting for trace %i\n", index);
    ret = sem_wait(&signal);
    if(ret < 0)
    {
        err("Failed to wait on consumer signal\n");
        return -errno;
    }

    debug("Came out of wait for trace %i\n", index);
    sem_destroy(&signal);

    // already unlinked by push
    free(request);
    return 0;
}

int __search_for_entry(struct list_head *queue,
                       port_t port, struct trace_set *ts, int index,
                       struct trace **res, struct trace_cache **cache)
{
    int ret;
    struct __waiter_entry *curr_waiter = NULL;

    if(!queue || !ts || !res)
    {
        err("Invalid args\n");
        return -EINVAL;
    }

    list_for_each_entry(curr_waiter, queue, list)
    {
        if(curr_waiter->port == port && curr_waiter->set == ts)
            break;
    }

    if(curr_waiter->port == port &&
       curr_waiter->set == ts)
    {
        ret = sem_wait(&curr_waiter->lock);
        if(ret < 0)
        {
            err("Failed to wait on queue entry lock\n");
            return -errno;
        }

        ret = tc_lookup(curr_waiter->available, index, res, false);
        if(ret < 0)
        {
            err("Failed to lookup trace in available cache\n")
            return ret;
        }

        if(!*res)
        {
            ret = __wait_for_entry(index, curr_waiter);
            if(ret < 0)
            {
                err("Failed to wait for entry to be pushed\n");
                return ret;
            }

            ret = sem_wait(&curr_waiter->lock);
            if(ret < 0)
            {
                err("Failed to wait on queue entry lock\n");
                return -errno;
            }

            ret = tc_lookup(curr_waiter->available, index, res, false);
            if(ret < 0)
            {
                err("Failed to lookup trace in available cache\n")
                return ret;
            }

            if(!*res)
            {
                err("Failed to find requested trace after available signaled\n");
                return -EINVAL;
            }
        }

        *cache = curr_waiter->available;
        ret = sem_post(&curr_waiter->lock);
        if(ret < 0)
        {
            err("Failed to post to queue entry lock\n");
            return -errno;
        }

        return 0;
    }
    else
    {
        err("Unable to find registered waiter entry for given port and trace set\n");
        return -EINVAL;
    }
}

int __tfm_wait_on_get(struct trace *t)
{
    int ret;
    struct trace *trace;
    struct trace_cache *cache;

    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_state;

    ret = __search_for_entry(queue, tfm->port, t->owner,
                             TRACE_IDX(t), &trace, &cache);
    if(ret < 0)
    {
        err("Failed to search for trace entry\n");
        return ret;
    }

    ret = copy_title(t, trace);
    if(ret >= 0)
        ret = copy_data(t, trace);

    if(ret >= 0)
        ret = copy_samples(t, trace);

    if(ret < 0)
    {
        err("Failed to copy something\n");
        passthrough_free(t);
        return ret;
    }

    tc_deref(cache, TRACE_IDX(t), trace);
    return 0;
}

void __tfm_wait_on_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_wait_on(struct tfm **tfm, port_t port, size_t bufsize)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_wait_on);

    res->data = calloc(1, sizeof(struct tfm_wait_on));
    if(!res->data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->port = port;
    TFM_DATA(res)->bufsize = bufsize;

    *tfm = res;
    return 0;
}