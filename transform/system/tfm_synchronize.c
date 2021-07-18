#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"
#include "platform.h"
#include "list.h"

#include <errno.h>

#define TFM_DATA(tfm)   ((struct tfm_synchronize *) (tfm)->data)

struct __tfm_synchronize_entry
{
    struct list_head list;
    size_t index;
    int count, signalled;
    LT_SEM_TYPE signal;
};

struct tfm_synchronize
{
    LT_SEM_TYPE list_lock;
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
{
    // todo
}

int __stall(struct tfm_synchronize *tfm, size_t index)
{
    int ret;
    struct __tfm_synchronize_entry *curr, *new;
    bool existing = false;

    list_for_each_entry(curr, &tfm->waiting, struct __tfm_synchronize_entry, list)
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
        new->signalled = 0;
        new->count = 0;

        ret = p_sem_create(&new->signal, 0);
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

    sem_release(&tfm->list_lock);
    debug("Stalling index %zu\n", index);
    sem_acquire(&curr->signal);
    debug("Index %zu good to go\n", index);
    sem_acquire(&tfm->list_lock);

    curr->count--;
    if (curr->signalled)
        curr->signalled = 0;
    else
    {
        err("Signalled flag not set after wakeup\n");
        return -EINVAL;
    }

    if(curr->count == 0)
    {
        list_del(&curr->list);
        p_sem_destroy(&curr->signal);
        free(curr);
    }

    return 0;
}

int __synchronize(struct tfm_synchronize *tfm, size_t index)
{
    int ret;
    struct __tfm_synchronize_entry *curr, *new;
    bool wait = false, found = false;

    // first, check if we should stall
    sem_acquire(&tfm->list_lock);
    list_for_each_entry(curr, &tfm->requests, struct __tfm_synchronize_entry, list)
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
    list_for_each_entry(curr, &tfm->requests, struct __tfm_synchronize_entry, list)
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
        new->signalled = 0;
        new->count = 0;

        list_add_tail(&new->list, &curr->list);
        curr = new;
    }

    curr->count++;
    sem_release(&tfm->list_lock);
    return 0;
}

int __sync_finalize(struct tfm_synchronize *tfm, size_t index)
{
    struct __tfm_synchronize_entry *curr;
    bool found = false;

    // wake up any applicable stalled requests
    sem_acquire(&tfm->list_lock);
    list_for_each_entry(curr, &tfm->waiting, struct __tfm_synchronize_entry, list)
    {
        if (index + tfm->max_distance >= curr->index &&
            !curr->signalled)
        {
            curr->signalled = 1;
            sem_release(&curr->signal);
        }
    }

    // find ourselves in the list
    list_for_each_entry(curr, &tfm->requests, struct __tfm_synchronize_entry, list)
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
        err("Couldn't find index %zu in the request list\n", index);
        return -EINVAL;
    }

    sem_release(&tfm->list_lock);
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

    ret = passthrough(t);
    if(ret < 0)
    {
        err("Failed to passthrough trace\n");
        return ret;
    }

    ret = __sync_finalize(TFM_DATA(t->owner->tfm), TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to finalize\n");
        return ret;
    }

    return 0;
}

void __tfm_synchronize_free(struct trace *t)
{
    passthrough_free(t);
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
    ret = p_sem_create(&TFM_DATA(res)->list_lock, 1);
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
