#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"
#include "list.h"

#include <errno.h>

#define TFM_DATA(tfm)   ((struct tfm_synchronize *) (tfm)->data)

struct __tfm_synchronize_entry
{
    struct list_head list;
    size_t index;
    int count;
    sem_t signal;
};

struct tfm_synchronize
{
    sem_t list_lock;
    int max_distance;

    struct list_head requests;
    struct list_head waiting;
};

int __tfm_synchronize_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_synchronize_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_synchronize_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_synchronize_exit(struct trace_set *ts)
{}

int __stall(struct tfm_synchronize *tfm, size_t index)
{
    int ret;
    struct __tfm_synchronize_entry *curr, *new;
    bool existing = false;

    list_for_each_entry(curr, &tfm->waiting, list)
    {
        if(curr->index < index)
            continue;
        else if(curr->index == index)
        {
            existing = true;
            break;
        }
        else break;
    }

    if(!existing)
    {
        new = calloc(1, sizeof(struct __tfm_synchronize_entry));
        if(!new)
        {
            err("Failed to allocate new sync entry\n");
            return -ENOMEM;
        }

        LIST_HEAD_INIT_INLINE(new->list);
        new->index = index;
        new->count = 0;

        ret = sem_init(&new->signal, 0, 0);
        if(ret < 0)
        {
            err("Failed to initialize new sync semaphore\n");
            free(new);
            return -errno;
        }

        list_add_tail(&new->list, &curr->list);
        curr = new;
    }

    curr->count++;
    ret = sem_post(&tfm->list_lock);
    if(ret < 0)
    {
        err("Failed to post to list lock sem\n");
        return -errno;
    }

    debug("Stalling index %li\n", index);

    ret = sem_wait(&curr->signal);
    if(ret < 0)
    {
        err("Failed to wait on semaphore signal\n");
        return -errno;
    }

    debug("Index %li good to go\n", index);

    ret = sem_wait(&tfm->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on list lock sem\n");
        return -errno;
    }

    curr->count--;
    if(curr->count == 0)
    {
        list_del(&curr->list);
        sem_destroy(&curr->signal);
        free(curr);
    }

    return 0;
}

int __synchronize(struct tfm_synchronize *tfm, size_t index)
{
    int ret;
    struct __tfm_synchronize_entry *curr, *new;
    bool wait = false, found = false;

    ret = sem_wait(&tfm->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on list lock\n");
        return -errno;
    }

    // first, check if we should stall
    list_for_each_entry(curr, &tfm->requests, list)
    {
        if(curr->index + tfm->max_distance < index)
        {
            wait = true;
            break;
        }
    }

    // stall
    if(wait)
    {
        ret = __stall(tfm, index);
        if(ret < 0)
        {
            err("Failed to stall request\n");
            return -errno;
        }
    }

    // look for an entry, or create our own if need be
    list_for_each_entry(curr, &tfm->requests, list)
    {
        if(curr->index < index)
            continue;
        else if(curr->index == index)
        {
            found = true;
            break;
        }
        else break;
    }

    if(!found)
    {
        new = calloc(1, sizeof(struct __tfm_synchronize_entry));
        if(!new)
        {
            err("failed to allocate new sync entry\n");
            return -ENOMEM;
        }

        LIST_HEAD_INIT_INLINE(new->list);
        new->index = index;
        new->count = 0;

        list_add_tail(&new->list, &curr->list);
        curr = new;
    }

    curr->count++;
    ret = sem_post(&tfm->list_lock);
    if(ret < 0)
    {
        err("Failed to post to list lock sem\n");
        return -errno;
    }

    return 0;
}

int __finalize(struct tfm_synchronize *tfm, size_t index)
{
    int ret;
    struct __tfm_synchronize_entry *curr;
    bool found = false;

    ret = sem_wait(&tfm->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on list lock sem\n");
        return -errno;
    }

    // wake up any applicable stalled requests
    list_for_each_entry(curr, &tfm->waiting, list)
    {
        if(index + tfm->max_distance >= curr->index)
        {
            ret = sem_post(&curr->signal);
            if(ret < 0)
            {
                err("Failed to wake up waiting request\n");
                return -errno;
            }
        }
    }

    // find ourselves in the list
    list_for_each_entry(curr, &tfm->requests, list)
    {
        if(curr->index == index)
        {
            found = true;
            break;
        }
    }

    if(found)
    {
        curr->count--;
        if(curr->count == 0)
        {
            list_del(&curr->list);
            free(curr);
        }
    }
    else
    {
        err("Couldn't find index %li in the request list\n", index);
        return -EINVAL;
    }

    ret = sem_post(&tfm->list_lock);
    if(ret < 0)
    {
        err("Failed to post to list lock sem\n");
        return -errno;
    }

    return 0;
}

int __tfm_synchronize_get(struct trace *t)
{
    int ret;

    ret = __synchronize(TFM_DATA(t->owner->tfm), TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to synchronize\n");
        return ret;
    }

    ret = passthrough_all(t);
    if(ret < 0)
    {
        err("Failed to passthrough title\n");
        return ret;
    }

    ret = __finalize(TFM_DATA(t->owner->tfm), TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to finalize\n");
        return ret;
    }

    return 0;
}

void __tfm_synchronize_free(struct trace *t)
{
    passthrough_free_all(t);
}

int tfm_synchronize(struct tfm **tfm, int max_distance)
{
    int ret;
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_synchronize);

    res->data = calloc(1, sizeof(struct tfm_synchronize));
    if(!res->data)
    {
        err("Failed to allocate memory for transformation variables\n");
        ret = -ENOMEM;
        goto __free_res;
    }

    // todo this structure really needs to live in ts->tfm_state
    ret = sem_init(&TFM_DATA(res)->list_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize list lock\n");
        ret = -errno;
        goto __free_data;
    }

    TFM_DATA(res)->max_distance = max_distance;
    LIST_HEAD_INIT_INLINE(TFM_DATA(res)->requests);
    LIST_HEAD_INIT_INLINE(TFM_DATA(res)->waiting);

    *tfm = res;
    return 0;

__free_data:
    free(res->data);

__free_res:
    free(res);
    return ret;
}
