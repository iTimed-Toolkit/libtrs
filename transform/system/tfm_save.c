#include "transform.h"
#include "trace.h"
#include "list.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"
#include "platform.h"

#include <errno.h>
#include <string.h>

#define SENTINEL        SIZE_MAX

struct tfm_save_state
{
    size_t num_traces_written;
    size_t prev_next_trace;
    void *commit_data;
};

struct __commit_queue_entry
{
    struct list_head list;
    struct trace *trace;
    size_t prev_index;
};

struct __commit_queue
{
    LT_THREAD_TYPE handle;
    LT_SEM_TYPE sentinel;

    struct trace_set *ts;
    int thread_ret;

    LT_SEM_TYPE list_lock;
    struct list_head head;

    size_t written;
    bool sentinel_seen;
};

int index_order(void *node1, void *node2)
{
    struct __commit_queue_entry *entry1 = node1;
    struct __commit_queue_entry *entry2 = node2;

    if(entry1->prev_index == SENTINEL)
        return -1;

    if(entry2->prev_index == SENTINEL)
        return 1;

    return (int) entry2->prev_index - (int) entry1->prev_index;
}

void print_commit(void *data)
{
    struct __commit_queue_entry *entry = data;
    fprintf(stderr, " %zu (%p)\n", entry->prev_index, entry->trace);
}

int __list_create_entry(struct __commit_queue *queue,
                        struct __commit_queue_entry **res,
                        size_t prev_index)
{
    int count = 0;
    struct __commit_queue_entry *entry, *curr;

    if(!queue || !res)
    {
        err("Invalid commit queue or node\n");
        return -EINVAL;
    }

    entry = calloc(1, sizeof(struct __commit_queue_entry));
    if(!entry)
    {
        err("Failed to allocate commit queue entry\n");
        return -ENOMEM;
    }

    entry->prev_index = prev_index;

    sem_acquire(&queue->list_lock);
    list_for_each_entry(curr, &queue->head, struct __commit_queue_entry, list)
    {
        if(curr->prev_index > prev_index)
            break;
        else count++;
    }
    list_add_tail(&entry->list, &curr->list);
    debug("Appending prev_index %zu at spot %i\n", prev_index, count);

    sem_release(&queue->list_lock);
    *res = entry;
    return 0;
}

int __list_remove_entry(struct __commit_queue *queue,
                        struct __commit_queue_entry *entry)
{
    if(!queue || !entry)
    {
        err("Invalid commit queue or node\n");
        return -EINVAL;
    }

    sem_acquire(&queue->list_lock);
    list_del(&entry->list);
    free(entry);
    sem_release(&queue->list_lock);

    return 0;
}

int __commit_traces(struct __commit_queue *queue, struct list_head *write_head)
{
    int ret;
    size_t prev_index;

    struct trace *trace_to_commit;
    struct __commit_queue_entry *entry;

    while(!list_empty(write_head))
    {
        entry = list_entry(write_head->next, struct __commit_queue_entry, list);

        prev_index = entry->prev_index;
        trace_to_commit = entry->trace;

        list_del(&entry->list);
        free(entry);

        if(prev_index == SENTINEL)
        {
            debug("Encountered sentinel, setting num_traces %zu\n",
                  queue->written);

            queue->sentinel_seen = true;
            sem_acquire(&queue->list_lock);
            queue->ts->num_traces = queue->written;
            sem_release(&queue->list_lock);
            sem_release(&queue->sentinel);
        }
        else
        {
            if(!queue->sentinel_seen)
            {
                if(trace_to_commit->owner != queue->ts)
                {
                    err("Bad trace to commit -- unknown trace set\n");
                    return -EINVAL;
                }

                debug("Committing %s\n", trace_to_commit->title);

                trace_to_commit->index = queue->written;
                ret = queue->ts->backend->write(trace_to_commit);
                if(ret < 0)
                {
                    err("Failed to append trace to file\n");
                    return ret;
                }

                // make this an anonymous trace to ensure memory is actually freed
                trace_to_commit->owner = NULL;
                trace_free_memory(trace_to_commit);
                queue->written++;
            }
            else
            {
                err("Encountered trace to write after seeing sentinel\n");
                return -EINVAL;
            }
        }
    }

    return 0;
}

LT_THREAD_FUNC(__commit_thread, arg)
{
    int ret;
    size_t count;

    struct __commit_queue *queue = arg;
    struct __commit_queue_entry *curr;
    struct tfm_save_state *tfm_data;
    LIST_HEAD(write_head);

    while(1)
    {
        p_sleep(1000);
        sem_acquire(&queue->list_lock);

        if(queue->ts == NULL)
        {
            debug("Commit thread exiting cleanly\n");
            queue->thread_ret = 0;
            return NULL;
        }

        // examine the commit list
        tfm_data = queue->ts->tfm_state;

        count = 0;
        list_for_each_entry(curr, &queue->head, struct __commit_queue_entry, list)
        {
            if(curr->trace || curr->prev_index == SENTINEL)
                count++;
            else break;
        }

        list_cut_before(&write_head, &queue->head, &curr->list);
        sem_release(&queue->list_lock);

        // write the collected batch
        if(!list_empty(&write_head))
        {
            warn("Writing %zu traces\n", count);
            ret = __commit_traces(queue, &write_head);
            if(ret < 0)
            {
                err("Failed to commit traces\n");
                queue->thread_ret = ret;
                return NULL;
            }

            // update global written counter
            sem_with(&queue->list_lock, tfm_data->num_traces_written += count);
        }
    }
}

int __tfm_save_init(struct trace_set *ts)
{
    int ret;
    struct __commit_queue *queue;
    struct tfm_save_state *tfm_data;

    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = UNKNOWN_NUM_TRACES;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;

    tfm_data = calloc(1, sizeof(struct tfm_save_state));
    if(!tfm_data)
    {
        err("Failed to allocate transformation data\n");
        return -ENOMEM;
    }

    tfm_data->num_traces_written = 0;
    tfm_data->prev_next_trace = 0;

    ret = create_backend(ts, ts->tfm->data);
    if(ret < 0)
    {
        err("Failed to create backend\n");
        goto __free_tfm_data;
    }

    ret = ts->backend->create(ts);
    if(ret < 0)
    {
        err("Failed to initialize new backend\n");
        goto __close_backend;
    }

    // initialize the commit queue
    queue = calloc(1, sizeof(struct __commit_queue));
    if(!queue)
    {
        err("Failed to allocate commit queue struct\n");
        goto __close_backend;
    }

    queue->ts = ts;
    queue->thread_ret = 0;
    LIST_HEAD_INIT_INLINE(queue->head);
    queue->written = 0;
    queue->sentinel_seen = false;

    ret = p_sem_create(&queue->sentinel, 0);
    if(ret < 0)
    {
        err("Failed to initialize sentinel semaphore\n");
        goto __free_commit_queue;
    }

    ret = p_sem_create(&queue->list_lock, 1);
    if(ret < 0)
    {
        err("Failed to initialize queue list lock\n");
        goto __destroy_sentinel_event;
    }

    ret = p_thread_create(&queue->handle, __commit_thread, queue);
    if(ret < 0)
    {
        err("Failed to create commit thread\n");
        goto __destroy_queue_list;
    }

    tfm_data->commit_data = queue;
    ts->tfm_state = tfm_data;
    return 0;

__destroy_queue_list:
    p_sem_destroy(&queue->list_lock);

__destroy_sentinel_event:
    p_sem_destroy(&queue->sentinel);

__free_commit_queue:
    free(queue);

__close_backend:
    ts->backend->close(ts);

__free_tfm_data:
    free(tfm_data);

    return ret;
}

int __tfm_save_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_save_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_save_exit(struct trace_set *ts)
{
    int ret;
    struct tfm_save_state *tfm_data = ts->tfm_state;
    struct __commit_queue *queue = tfm_data->commit_data;

    // first, wait for commit list to drain
    while(1)
    {
        sem_acquire(&queue->list_lock);
        if(list_empty(&queue->head))
        {
            debug("Commit queue is drained\n");
            break;
        }
        sem_release(&queue->list_lock);
    }

    // kill the commit threads
    queue->ts = NULL;
    p_thread_join(queue->handle);

    p_sem_destroy(&queue->list_lock);
    free(queue);

    // finalize headers
    ts->num_traces = tfm_data->num_traces_written;
    ret = ts->backend->close(ts);
    if(ret < 0)
        err("Failed to close backend for trace set\n");

    free(tfm_data);
    ts->tfm_state = NULL;
}

int __render_to_index(struct trace_set *ts, size_t index)
{
    int ret;
    size_t prev_index;

    struct tfm_save_state *tfm_data = ts->tfm_state;
    struct __commit_queue *queue = tfm_data->commit_data;
    struct __commit_queue_entry *entry;
    struct trace *t_prev, *t_result;

    while(1)
    {
        sem_acquire(&queue->list_lock);
        if(index < tfm_data->num_traces_written)
        {
            debug("Index %zu < written %zu, exiting\n", index, tfm_data->num_traces_written);
            sem_release(&queue->list_lock); break;
        }

        prev_index = tfm_data->prev_next_trace++;
        sem_release(&queue->list_lock);

        debug("Checking prev_index %zu (want %zu)\n", prev_index, index);
        if(prev_index >= ts_num_traces(ts->prev))
        {
            // send a sentinel down the pipeline
            debug("Index %zu out of bounds for previous trace set\n", prev_index);

            ret = __list_create_entry(queue, &entry, SENTINEL);
            if(ret < 0)
            {
                err("Failed to add sentinel to synchronization list\n");
                return ret;
            }

            return 1;
        }

        ret = __list_create_entry(queue, &entry, prev_index);
        if(ret < 0)
        {
            err("Failed to add prev index to synchronization list\n");
            return ret;
        }

        ret = trace_get(ts->prev, &t_prev, prev_index);
        if(ret < 0)
        {
            err("Failed to get trace from previous trace set\n");
            return ret;
        }

        if(!t_prev->samples ||
           (ts->prev->title_size != 0 && !t_prev->title) ||
           (ts->prev->data_size != 0 && !t_prev->data))
        {
            debug("prev_index %zu not a valid index\n", prev_index);
            trace_free(t_prev);
            __list_remove_entry(queue, entry);
            continue;
        }

        debug("prev_index %zu is a valid index, appending\n", prev_index);
        ret = trace_copy(&t_result, t_prev);
        t_result->owner = ts;

        trace_free(t_prev);
        if(ret < 0)
        {
            err("Failed to create new trace from previous\n");
            return ret;
        }

        entry->trace = t_result;
        if(queue->thread_ret < 0)
        {
            err("Detected error in commit thread\n");
            trace_free(t_result);
            __list_remove_entry(queue, entry);
            return queue->thread_ret;
        }
    }

    return 0;
}

int __tfm_save_get(struct trace *t)
{
    int ret;
    struct tfm_save_state *tfm_data = t->owner->tfm_state;
    struct __commit_queue *queue = tfm_data->commit_data;

    debug("Getting trace %zu\n", TRACE_IDX(t));
    if(TRACE_IDX(t) >= tfm_data->num_traces_written)
    {
        ret = __render_to_index(t->owner, TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to render trace set to index %zu\n", TRACE_IDX(t));
            return ret;
        }
        else if(ret == 1)
        sem_acquire(&queue->sentinel);
    }

    debug("Reading trace %zu from file\n", TRACE_IDX(t));
    if(TRACE_IDX(t) < t->owner->num_traces)
    {
        ret = t->owner->backend->read(t);
        if(ret < 0)
        {
            err("Failed to read trace %zu from file\n", TRACE_IDX(t));
            return ret;
        }
    }
    else
    {
        debug("Setting trace %zu to null\n", TRACE_IDX(t));
        t->title = NULL;
        t->data = NULL;
        t->samples = NULL;
    }
    return 0;
}

void __tfm_save_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_save(struct tfm **tfm, char *path)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_save);
    res->data = path;

    *tfm = res;
    return 0;
}
