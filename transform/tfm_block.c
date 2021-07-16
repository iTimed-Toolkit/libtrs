#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"
#include "list.h"
#include "platform.h"

#include <string.h>
#include <errno.h>

#define TITLE_SIZE      128
#define LIST_LENGTH     16


struct __tfm_block_block
{
    struct list_head list;
    bool done;
    size_t src_index, res_index;
    int num_out;

    void *block;
};

struct tfm_block_state
{
    size_t next_index;
    size_t done_index;
    size_t nblocks;

    LT_SEM_TYPE lock;
    struct list_head blocks;
};

#define TFM_DATA(tfm)   ((struct block_args *) (tfm)->data)

int __tfm_block_init(struct trace_set *ts)
{
    int ret;
    struct tfm_block_state *state;
    struct block_args *tfm = TFM_DATA(ts->tfm);

    ts->title_size = TITLE_SIZE;

    // these can be overloaded
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1.0f;

    // its okay to not know the number of traces,
    // but should at least have number of samples
    ts->num_traces = UNKNOWN_NUM_TRACES;
    ts->num_samples = -1;

    state = calloc(1, sizeof(struct tfm_block_state));
    if(!state)
    {
        err("Failed to allocate block state\n");
        return -ENOMEM;
    }

#if BLK_DEBUG
    state->next_index = BLK_DEBUG_FIRST;
#else
    state->next_index = 0;
#endif
    state->done_index = 0;
    state->nblocks = 0;
    LIST_HEAD_INIT_INLINE(state->blocks);

    ret = p_sem_create(&state->lock, 1);
    if(ret < 0)
    {
        err("Failed to initialize state semaphore\n");
        ret = -errno;
        goto __free_state;
    }

    ret = tfm->consumer_init(ts, tfm->arg);
    if(ret < 0 || ts->num_samples == -1 || ts->title_size != TITLE_SIZE)
    {
        err("Failed to initialize consumer\n");
        goto __destroy_sem;
    }

    ts->tfm_state = state;
    return 0;

__destroy_sem:
    p_sem_destroy(&state->lock);

__free_state:
    free(state);

    return ret;
}

int __tfm_block_init_waiter(struct trace_set *ts, port_t port)
{
    struct block_args *tfm = TFM_DATA(ts->prev->tfm);

    if(tfm->consumer_init_waiter)
        return tfm->consumer_init_waiter(ts, port, tfm->arg);
    else
    {
        err("No ports to register\n");
        return -EINVAL;
    }
}

size_t __tfm_block_trace_size(struct trace_set *ts)
{
    return ts->num_samples * sizeof(float) +
           ts->data_size + ts->title_size +
           sizeof(struct trace);
}

void __tfm_block_exit(struct trace_set *ts)
{
    // todo call consumer exit, clear list? etc
}

int __block_accumulate(struct tfm_block_state *state,
                       struct block_args *tfm,
                       struct trace *curr_trace)
{
    int ret;
    bool found = false;
    struct __tfm_block_block *curr, *new;

    if(tfm->criteria != DONE_SINGULAR)
    {
        sem_acquire(&state->lock);
        list_for_each_entry(curr, &state->blocks, list)
        {
            if(tfm->trace_matches(curr_trace, curr->block, tfm->arg))
            {
                found = true;
                ret = tfm->accumulate(curr_trace, curr->block, tfm->arg);
                if(ret < 0)
                {
                    err("Failed to accumulate trace into block\n");
                    goto __unlock;
                }

                break;
            }
        }
    }

    if(!found)
    {
        new = calloc(1, sizeof(struct __tfm_block_block));
        if(!new)
        {
            err("Failed to allocate new meta-block entry\n");
            ret = -ENOMEM;
            goto __unlock;
        }

        LIST_HEAD_INIT_INLINE(new->list);
        new->done = false;
        new->src_index = TRACE_IDX(curr_trace);
        new->res_index = TRACE_IDX(curr_trace);
        new->num_out = 0;

        ret = tfm->initialize(curr_trace, &new->block, tfm->arg);
        if(ret < 0)
        {
            err("Failed to initialize new block\n");
            free(new);
            goto __unlock;
        }

        ret = tfm->accumulate(curr_trace, new->block, tfm->arg);
        if(ret < 0)
        {
            err("Failed to accumulate trace into new block\n");
            goto __unlock;
        }

        // need to lock only to append to list
        if(tfm->criteria == DONE_SINGULAR)
        sem_acquire(&state->lock);

        list_add_tail(&new->list, &state->blocks);
        state->nblocks++;

        if(tfm->criteria == DONE_SINGULAR)
        {
            new->done = true;
            new->res_index = state->done_index++;
        }
        else if(tfm->criteria == DONE_LISTLEN && state->nblocks == LIST_LENGTH)
        {
            curr = list_first_entry(&state->blocks, struct __tfm_block_block, list);
            if(!curr->done)
            {
                curr->done = true;
                curr->res_index = state->done_index++;
            }
        }

        if(tfm->criteria == DONE_SINGULAR)
        sem_release(&state->lock);
    }

    ret = 0;
__unlock:
    if(tfm->criteria != DONE_SINGULAR)
    sem_release(&state->lock);
    return ret;
}

int __block_finalize(struct tfm_block_state *state,
                     struct block_args *tfm,
                     struct trace *res)
{
    int ret;
    bool found = false, delete = false;
    struct __tfm_block_block *curr;

    if(TRACE_IDX(res) >= state->done_index)
    {
        err("Called too early\n");
        return -EINVAL;
    }

    sem_acquire(&state->lock);
    debug("Finalize for trace %li\n", TRACE_IDX(res));
    list_for_each_entry(curr, &state->blocks, list)
    {
        debug("Found a block with index %li\n", curr->res_index);
        if(curr->res_index == TRACE_IDX(res) && curr->done)
        {
            debug("This is the correct block!\n");
            found = true;
            break;
        }
    }

    if(found)
    {
        ret = tfm->finalize(res, curr->block, tfm->arg);
        if(ret < 0)
        {
            err("Failed to finalize block into result trace\n");
            goto __unlock;
        }

        // this block has more traces to finalize: signal the next one
        if(ret == 1)
        {
            curr->res_index = state->done_index++;
            curr->num_out++;
        }
        else delete = true;
    }
    else
    {
        err("Failed to find correct result block in list for index %li and in cache\n",
            TRACE_IDX(res));
        ret = -EINVAL;
        goto __unlock;
    }

    if(res->title)
    {
        if(strlen(res->title) >= TITLE_SIZE)
        {
            err("Consumer set a too-long title value\n");
            ret = -EINVAL;
            goto __unlock;
        }
    }
    else
    {
        res->title = calloc(TITLE_SIZE, sizeof(char));
        if(!res->title)
        {
            err("Failed to allocate title memory\n");
            ret = -ENOMEM;
            goto __unlock;
        }
        snprintf(res->title, TITLE_SIZE, "Block %li.%i\n",
                 curr->src_index, curr->num_out - 1);
    }

    if(delete)
    {
        state->nblocks--;
        list_del(&curr->list);
        free(curr);
    }

__unlock:
    sem_release(&state->lock);
    return ret;
}

int __tfm_block_get(struct trace *t)
{
    int ret;
    size_t index;

    struct trace *curr_trace;
    struct block_args *tfm = TFM_DATA(t->owner->tfm);
    struct tfm_block_state *state = t->owner->tfm_state;

    debug("Getting index %li\n", TRACE_IDX(t));
    while(1)
    {
        if(TRACE_IDX(t) < state->done_index)
            break;

        index = __atomic_fetch_add(&state->next_index, 1, __ATOMIC_RELAXED);
        if(index >= ts_num_traces(t->owner->prev))
        {
            // todo halting condition
        }

        ret = trace_get(t->owner->prev, &curr_trace, index);
        if(ret < 0)
        {
            err("Failed to get trace from previous set\n");
            return ret;
        }

        if(tfm->trace_interesting(curr_trace, tfm->arg))
        {
            ret = __block_accumulate(state, tfm, curr_trace);
            if(ret < 0)
            {
                err("Failed to accumulate trace into a block\n");
                return ret;
            }
        }

        trace_free(curr_trace);
    }

    debug("About to finalize for trace %li\n", TRACE_IDX(t));
    return __block_finalize(state, tfm, t);
}

void __tfm_block_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_block(struct tfm **tfm, struct block_args *args)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_block);

    res->data = calloc(1, sizeof(struct block_args));
    if(!res->data)
    {
        free(res);
        return -ENOMEM;
    }

    memcpy(res->data, args, sizeof(struct block_args));
    *tfm = res;
    return 0;
}