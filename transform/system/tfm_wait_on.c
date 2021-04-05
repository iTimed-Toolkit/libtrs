#include "transform.h"
#include "trace.h"
#include "list.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#define TFM_DATA(tfm)   ((struct tfm_wait_on *) (tfm)->tfm_data)

struct tfm_wait_on
{
    port_t port;
};

struct __trace_entry
{
    struct list_head list;
    int index;

    char *title;
    uint8_t *data;
    float *samples;

    int ref_title,
            ref_data,
            ref_samples;
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

    struct list_head traces_available;
    struct list_head traces_wanted;
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
    struct __trace_entry *entry, *curr_entry;
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

    list_for_each_entry(curr_waiter, queue, list)
    {
        if(curr_waiter->port == port)
        {
            entry = calloc(1, sizeof(struct __trace_entry));
            if(!entry)
            {
                err("Failed to allocate entry for new trace\n");
                return -ENOMEM;
            }

            LIST_HEAD_INIT_INLINE(entry->list);
            entry->index = index;

            if(pushed_title)
            {
                entry->ref_title = 0;
                entry->title = calloc(curr_waiter->set->title_size, sizeof(char));
                if(!entry->title)
                {
                    err("Failed to allocate entry title\n");
                    goto __free_entry;
                }

                memcpy(entry->title, pushed_title,
                       curr_waiter->set->title_size * sizeof(char));
            }
            else entry->title = NULL;

            if(pushed_data)
            {
                entry->ref_data = 0;
                entry->data = calloc(curr_waiter->set->data_size, sizeof(uint8_t));
                if(!entry->data)
                {
                    err("Failed to allocate entry data\n");
                    goto __free_entry;
                }

                memcpy(entry->data, pushed_data,
                       curr_waiter->set->data_size * sizeof(uint8_t));
            }
            else entry->data = NULL;

            if(pushed_samples)
            {
                entry->ref_samples = 0;
                entry->samples = calloc(curr_waiter->set->num_samples, sizeof(float));
                if(!entry->samples)
                {
                    err("Failed to allocate entry samples\n");
                    goto __free_entry;
                }

                memcpy(entry->samples, pushed_samples,
                       curr_waiter->set->num_samples * sizeof(float));
            }
            else entry->samples = NULL;

            ret = sem_wait(&curr_waiter->lock);
            if(ret < 0)
            {
                err("Failed to wait on entry lock\n");
                return -errno;
            }

            list_for_each_entry(curr_entry, &curr_waiter->traces_available, list)
            {
                if(curr_entry->index > index)
                    break;
            }
            list_add_tail(&entry->list, &curr_entry->list);
            entry = NULL;

            // wake up any consumers
            list_for_each_entry_safe(curr_req, n_req, &curr_waiter->traces_wanted, list)
            {
                if(curr_req->index < index) continue;
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

                    free(curr_req);
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

__free_entry:
    if(entry->title)
        free(entry->title);

    if(entry->data)
        free(entry->data);

    if(entry->samples)
        free(entry->samples);

    free(entry);
    return -ENOMEM;
}

int __tfm_wait_on_init(struct trace_set *ts)
{
    int ret;

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
        if(ts->prev->tfm_next != __tfm_wait_on_push)
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

    ret = sem_init(&entry->lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize entry lock\n");
        ret = -errno;
        goto __free_entry;
    }

    entry->port = tfm->port;
    entry->set = ts;
    LIST_HEAD_INIT_INLINE(entry->traces_available);
    LIST_HEAD_INIT_INLINE(entry->traces_wanted);

    list_for_each_entry(curr, queue, list)
    {
        if(curr->port > tfm->port)
            break;
    }
    list_add_tail(&entry->list, queue);

    ts->tfm_data = queue;
    ts->prev->tfm_next_arg = queue;
    ts->prev->tfm_next = __tfm_wait_on_push;
    return 0;

__free_entry:
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
    return ts_trace_size(ts->prev);
}

int __search_for_entry(struct list_head *queue,
                       port_t port, struct trace_set *ts, int index,
                       struct __trace_entry **res)
{
    int ret;
    bool found;

    struct __waiter_entry *curr_waiter = NULL;
    struct __trace_entry *curr_trace;
    struct __request_entry *request, *curr_request;
    sem_t signal;

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

        found = false;
        list_for_each_entry(curr_trace, &curr_waiter->traces_available, list)
        {
            if(curr_trace->index < index) continue;
            else if(curr_trace->index > index) break;
            else
            {
                debug("Found trace %i\n", index);
                *res = curr_trace;
                found = true;
                break;
            }
        }

        if(!found)
        {
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
        }

        ret = sem_post(&curr_waiter->lock);
        if(ret < 0)
        {
            err("Failed to post to queue entry lock\n");
            return -errno;
        }

        if(!found)
        {
            debug("Waiting for trace %i\n", index);

            ret = sem_wait(&signal);
            if(ret < 0)
            {
                err("Failed to wait on consumer signal\n");
                return -errno;
            }

            debug("Came out of wait for trace %i\n", index);
            sem_destroy(&signal);
            ret = sem_wait(&curr_waiter->lock);
            if(ret < 0)
            {
                err("Failed to wait on queue entry lock\n");
                return -errno;
            }

            list_for_each_entry(curr_trace, &curr_waiter->traces_available, list)
            {
                if(curr_trace->index < index) continue;
                else if(curr_trace->index > index)
                {
                    err("Failed to find requested trace after available signaled\n");
                    return -EINVAL;
                }
                else
                {
                    *res = curr_trace;
                    break;
                }
            }

            ret = sem_post(&curr_waiter->lock);
            if(ret < 0)
            {
                err("Failed to post to queue entry lock\n");
                return -errno;
            }
        }

        return 0;
    }
    else
    {
        err("Unable to find registered waiter entry for given port and trace set\n");
        return -EINVAL;
    }
}

int __tfm_wait_on_title(struct trace *t, char **title)
{
    int ret;
    struct __trace_entry *entry;
    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_data;

    ret = __search_for_entry(queue, tfm->port, t->owner,
                             TRACE_IDX(t), &entry);
    if(ret < 0)
    {
        err("Failed to search for trace entry\n");
        return ret;
    }

    if(entry->title)
        __atomic_fetch_add(&entry->ref_title, 1, __ATOMIC_RELAXED);

    *title = entry->title;
    return 0;
}

int __tfm_wait_on_data(struct trace *t, uint8_t **data)
{
    int ret;
    struct __trace_entry *entry;
    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_data;

    ret = __search_for_entry(queue, tfm->port, t->owner,
                             TRACE_IDX(t), &entry);
    if(ret < 0)
    {
        err("Failed to search for trace entry\n");
        return ret;
    }

    if(entry->data)
        __atomic_fetch_add(&entry->ref_data, 1, __ATOMIC_RELAXED);

    *data = entry->data;
    return 0;
}

int __tfm_wait_on_samples(struct trace *t, float **samples)
{
    int ret;
    struct __trace_entry *entry;
    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_data;

    ret = __search_for_entry(queue, tfm->port, t->owner,
                             TRACE_IDX(t), &entry);
    if(ret < 0)
    {
        err("Failed to search for trace entry\n");
        return ret;
    }

    if(entry->samples)
        __atomic_fetch_add(&entry->ref_samples, 1, __ATOMIC_RELAXED);

    *samples = entry->samples;
    return 0;
}

void __tfm_wait_on_exit(struct trace_set *ts)
{
    struct __waiter_entry *curr;
    struct tfm_wait_on *tfm = TFM_DATA(ts->tfm);
    struct list_head *queue = ts->tfm_data;

    list_for_each_entry(curr, queue, list)
    {
        if(curr->port == tfm->port && curr->set == ts)
            break;
    }

    if(curr->port == tfm->port && curr->set == ts)
    {
        // todo: wait for consumer list to drain and then free everything in buffers?
        list_del(&curr->list);
    }
    else err("Unable to find registered waiter entry for given port and trace set\n");
}

typedef enum
{
    KIND_TITLE = 0,
    KIND_DATA,
    KIND_SAMPLES
} tfm_wait_on_kind_t;

int __deref_and_free_entry(struct list_head *queue,
                           port_t port, struct trace_set *ts, int index,
                           tfm_wait_on_kind_t kind)
{
    int ret;
    struct __waiter_entry *curr_waiter = NULL;
    struct __trace_entry *curr_trace;

    if(!queue || !ts)
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

        list_for_each_entry(curr_trace, &curr_waiter->traces_available, list)
        {
            if(curr_trace->index < index) continue;
            else if(curr_trace->index > index)
            {
                err("Specified index not found\n");
                return -EINVAL;
            }
            else
            {
                switch(kind)
                {
                    case KIND_TITLE:
                        curr_trace->ref_title--;
                        if(curr_trace->ref_title == 0 &&
                           curr_trace->title)
                        {
                            free(curr_trace->title);
                            curr_trace->title = NULL;
                        }
                        break;

                    case KIND_DATA:
                        curr_trace->ref_data--;
                        if(curr_trace->ref_data == 0 &&
                           curr_trace->data)
                        {
                            free(curr_trace->data);
                            curr_trace->data = NULL;
                        }
                        break;

                    case KIND_SAMPLES:
                        curr_trace->ref_samples--;
                        if(curr_trace->ref_samples == 0 &&
                           curr_trace->samples)
                        {
                            free(curr_trace->samples);
                            curr_trace->samples = NULL;
                        }
                        break;

                    default:
                        err("Invalid trace data kind\n");
                        return -EINVAL;
                }

                if(!curr_trace->title &&
                   !curr_trace->data &&
                   !curr_trace->samples)
                {
                    list_del(&curr_trace->list);
                    free(curr_trace);
                }

                break;
            }
        }

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

void __tfm_wait_on_free_title(struct trace *t)
{
    int ret;
    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_data;

    ret = __deref_and_free_entry(queue, tfm->port, t->owner,
                                 TRACE_IDX(t), KIND_TITLE);
    if(ret < 0)
        err("Failed to deref and free trace title\n");
}

void __tfm_wait_on_free_data(struct trace *t)
{
    int ret;
    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_data;

    ret = __deref_and_free_entry(queue, tfm->port, t->owner,
                                 TRACE_IDX(t), KIND_DATA);
    if(ret < 0)
        err("Failed to deref and free trace title\n");
}

void __tfm_wait_on_free_samples(struct trace *t)
{
    int ret;
    struct tfm_wait_on *tfm = TFM_DATA(t->owner->tfm);
    struct list_head *queue = t->owner->tfm_data;

    ret = __deref_and_free_entry(queue, tfm->port, t->owner,
                                 TRACE_IDX(t), KIND_SAMPLES);
    if(ret < 0)
        err("Failed to deref and free trace title\n");
}

int tfm_wait_on(struct tfm **tfm, port_t port)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_wait_on);

    res->tfm_data = calloc(1, sizeof(struct tfm_wait_on));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->port = port;
    *tfm = res;
    return 0;
}