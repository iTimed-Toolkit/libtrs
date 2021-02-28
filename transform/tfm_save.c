#include "transform.h"
#include "libtrs.h"

#include "__tfm_internal.h"
#include "__libtrs_internal.h"

#include <errno.h>
#include <string.h>
#include <pthread.h>

#define TFM_DATA(tfm)   ((struct tfm_save *) (tfm)->tfm_data)

struct tfm_save
{
    char *prefix;
};

struct __commit_queue_entry
{
    struct trace *trace;
    size_t prev_index;
};

struct __commit_queue
{
    pthread_t handle;
    sem_t event;

    sem_t list_lock;
    struct list *head;
};


int index_order(void *node1, void *node2)
{
    struct __commit_queue_entry *entry1 = node1;
    struct __commit_queue_entry *entry2 = node2;

    return (int) entry2->prev_index - (int) entry1->prev_index;
}

int __list_create_entry(struct __commit_queue *queue,
                        struct list **node,
                        size_t prev_index)
{
    int ret;
    struct __commit_queue_entry *entry;

    // todo parameter checking

    entry = calloc(1, sizeof(struct __commit_queue_entry));
    if(!entry)
    {
        err("Failed to allocate commit queue entry\n");
        return -ENOMEM;
    }

    entry->prev_index = prev_index;

    ret = sem_wait(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on commit queue lock\n");
        return -errno;
    }

    ret = list_create_node(node, entry);
    if(ret < 0)
    {
        err("Failed to allocate list node\n");
        return ret;
    }

    ret = list_link_single(&queue->head, *node, index_order);
    if(ret < 0)
    {
        err("Failed to link new node into list\n");
        goto __fail_free_node;
    }

    ret = sem_post(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to post to commit queue lock\n");
        return -errno;
    }

    return 0;

__fail_free_node:
    list_free_node(*node);
    *node = NULL;
    return ret;
}

int __list_remove_entry(struct __commit_queue *queue,
                        struct list *node)
{
    int ret;

    ret = sem_wait(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on commit queue lock\n");
        return -errno;
    }

    ret = list_unlink_single(&queue->head, node);
    if(ret < 0)
    {
        err("Failed to unlock node from list\n");
        return ret;
    }

    free(list_get_data(node));
    list_free_node(node);

    ret = sem_post(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to post to commit queue lock\n");
        return -errno;
    }

    return 0;
}

int __append_trace_to_file(struct trace *t)
{
    int i;
    int ret;
    size_t written;

    size_t temp_len;
    void *temp = NULL;

    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    warn("Appending trace %li to file\n", TRACE_IDX(t));

    ret = fseek(t->owner->ts_file, t->start_offset, SEEK_SET);
    if(ret < 0)
    {
        err("Failed to seek to end of trace set file\n");
        return -EIO;
    }

    // todo avx accelerate?
    switch(t->owner->datatype)
    {
        case DT_BYTE:
            temp_len = ts_num_samples(t->owner) * sizeof(char);
            temp = calloc(t->owner->num_samples, sizeof(char));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((char *) temp)[i] = (char) (t->buffered_samples[i] / t->owner->yscale);
            break;

        case DT_SHORT:
            temp_len = ts_num_samples(t->owner) * sizeof(short);
            temp = calloc(t->owner->num_samples, sizeof(short));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((short *) temp)[i] = (short) (t->buffered_samples[i] / t->owner->yscale);
            break;

        case DT_INT:
            temp_len = ts_num_samples(t->owner) * sizeof(int);
            temp = calloc(t->owner->num_samples, sizeof(int));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((int *) temp)[i] = (int) (t->buffered_samples[i] / t->owner->yscale);
            break;

        case DT_FLOAT:
            temp_len = ts_num_samples(t->owner) * sizeof(float);
            temp = calloc(t->owner->num_samples, sizeof(float));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((float *) temp)[i] = t->buffered_samples[i] / t->owner->yscale;
            break;

        case DT_NONE:
        default:
            err("Bad trace set datatype %i\n", t->owner->datatype);
            return -EINVAL;
    }

    if(!temp)
    {
        err("Failed to allocate temporary memory\n");
        return -ENOMEM;
    }

    if(t->buffered_title)
    {
        written = fwrite(t->buffered_title, 1, t->owner->title_size, t->owner->ts_file);
        if(written != t->owner->title_size)
        {
            err("Failed to write all bytes of title to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    if(t->buffered_data)
    {
        written = fwrite(t->buffered_data, 1, t->owner->data_size, t->owner->ts_file);
        if(written != t->owner->data_size)
        {
            err("Failed to write all bytes of data to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    written = fwrite(temp, 1, temp_len, t->owner->ts_file);
    if(written != temp_len)
    {
        err("Failed to write all bytes of samples to file\n");
        ret = -EIO;
        goto __free_temp;
    }

    fflush(t->owner->ts_file);

    ret = 0;
__free_temp:
    free(temp);
    return ret;

}

void print_commit(void *data)
{
    struct __commit_queue_entry *entry = data;
    fprintf(stderr, " %li (%p)\n", entry->prev_index, entry->trace);
}

void *__commit_thread(void *arg)
{
    int ret;
    size_t written = 0;
    struct __commit_queue *queue = arg;
    struct __commit_queue_entry *entry;

    struct trace *trace_to_commit;
    struct list *node;

    while(1)
    {
        ret = sem_wait(&queue->event);
        if(ret < 0)
        {
            err("Commit thread failed to wait on event signal\n");
            pthread_exit(NULL);
        }

        while(1)
        {
            ret = sem_wait(&queue->list_lock);
            if(ret < 0)
            {
                err("Commit thread failed to wait on list lock\n");
                pthread_exit(NULL);
            }

            if(queue->head)
            {
                node = queue->head;
                entry = list_get_data(queue->head);

                trace_to_commit = entry->trace;
                if(trace_to_commit)
                {
                    list_unlink_single(&queue->head, node);
                    list_free_node(node);
                    free(entry);
                }
            }

            if(written % 10000 == 0)
            {
                list_dump(queue->head, print_commit);
            }

            ret = sem_post(&queue->list_lock);
            if(ret < 0)
            {
                err("Commit thread failed to post to list lock\n");
                pthread_exit(NULL);
            }

            if(trace_to_commit)
            {
                ret = sem_wait(&trace_to_commit->owner->file_lock);
                if(ret < 0)
                {
                    err("Failed to wait on trace set file lock\n");
                    pthread_exit(NULL);
                }

                trace_to_commit->start_offset =
                        trace_to_commit->owner->trace_start +
                        written * trace_to_commit->owner->trace_length;

                ret = __append_trace_to_file(trace_to_commit);
                if(ret < 0)
                {
                    err("Failed to append trace to file\n");
                    pthread_exit(NULL);
                }

                written++;
                __atomic_store_n(&trace_to_commit->owner->num_traces_written,
                                 written, __ATOMIC_RELEASE);

                ret = sem_post(&trace_to_commit->owner->file_lock);
                if(ret < 0)
                {
                    err("Failed to post to trace set file lock\n");
                    pthread_exit(NULL);
                }

                trace_free_memory(trace_to_commit);
                trace_to_commit = NULL;
            }
        }
    }
}

int __tfm_save_init(struct trace_set *ts)
{
    int ret;
    char fname[256];
    struct tfm_save *tfm = TFM_DATA(ts->tfm);
    struct __commit_queue *queue;

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

    ts->num_traces_written = 0;
    ts->prev_next_trace = 0;

    ret = snprintf(fname, sizeof(fname), "%s_%li.trs",
                   tfm->prefix, ts->set_id);

    if(ret < 0 || ret >= sizeof(fname))
    {
        err("Trace set prefix too long\n");
        return -EINVAL;
    }

    // initialize the new file
    ts->ts_file = fopen(fname, "wb+");
    if(!ts->ts_file)
    {
        err("Failed to open trace set file %s\n", fname);
        return -EIO;
    }

    ret = write_default_headers(ts);
    if(ret < 0)
    {
        err("Failed to write default headers\n");
        goto __close_ts_file;
    }

    ret = write_inherited_headers(ts);
    if(ret < 0)
    {
        err("Failed to write inherited headers\n");
        goto __close_ts_file;
    }

    ret = fseek(ts->ts_file, 0, SEEK_SET);
    if(ret < 0)
    {
        err("Failed to seek trace set file to beginning\n");
        goto __close_ts_file;
    }

    ret = read_headers(ts);
    if(ret < 0)
    {
        err("Failed to read recently written headers\n");
        goto __close_ts_file;
    }

    ret = sem_init(&ts->file_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize file lock semaphore: %s\n", strerror(errno));
        ret = -errno;
        goto __free_headers;
    }

    // initialize the commit queue
    queue = calloc(1, sizeof(struct __commit_queue));
    if(!queue)
    {
        err("Failed to allocate commit queue struct\n");
        goto __free_headers;
    }

    ret = sem_init(&queue->event, 0, 0);
    if(ret < 0)
    {
        err("Failed to initialize queue event semaphore\n");
        goto __free_commit_queue;
    }

    ret = sem_init(&queue->list_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize queue list lock\n");
        goto __destroy_queue_event;
    }

    ret = pthread_create(&queue->handle, NULL,
                         __commit_thread, queue);
    if(ret < 0)
    {
        err("Failed to create commit thread\n");
        goto __destroy_queue_list;
    }

    ts->commit_data = queue;
    return 0;

__destroy_queue_list:
    sem_destroy(&queue->list_lock);

__destroy_queue_event:
    sem_destroy(&queue->event);

__free_commit_queue:
    free(queue);

__free_headers:
    free(ts->headers);

__close_ts_file:
    fclose(ts->ts_file);


    return ret;
}

size_t __tfm_save_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __create_new_trace_from_prev(struct trace **res, struct trace *prev)
{
    int ret;
    struct trace *t_result;

    t_result = calloc(1, sizeof(struct trace));
    if(!t_result)
    {
        err("Failed to allocate memory for trace\n");
        return -ENOMEM;
    }

    t_result->buffered_title = prev->buffered_title;
    t_result->buffered_data = prev->buffered_data;
    t_result->buffered_samples = prev->buffered_samples;

    t_result->buffered_title = calloc(prev->owner->title_size, sizeof(char));
    if(!t_result->buffered_title)
    {
        err("Failed to allocate memory for new trace title\n");
        ret = -ENOMEM;
        goto __fail_free_result;
    }

    t_result->buffered_data = calloc(prev->owner->data_size, sizeof(uint8_t));
    if(!t_result->buffered_data)
    {
        err("Failed to allocate memory for new trace data\n");
        ret = -ENOMEM;
        goto __fail_free_result;
    }

    t_result->buffered_samples = calloc(sizeof(float), prev->owner->num_samples);
    if(!t_result->buffered_samples)
    {
        err("Failed to allocate memory for new trace samples\n");
        ret = -ENOMEM;
        goto __fail_free_result;
    }

    memcpy(t_result->buffered_title,
           prev->buffered_title,
           prev->owner->title_size * sizeof(char));

    memcpy(t_result->buffered_data,
           prev->buffered_data,
           prev->owner->data_size * sizeof(uint8_t));

    memcpy(t_result->buffered_samples,
           prev->buffered_samples,
           prev->owner->num_samples * sizeof(float));

    *res = t_result;
    return 0;

__fail_free_result:
    trace_free_memory(t_result);
    return ret;
}

int __render_to_index(struct trace_set *ts, size_t index)
{
    int ret;
    size_t written, prev_index;

    struct __commit_queue *queue = ts->commit_data;
    struct __commit_queue_entry *entry;

    struct list *node = NULL;
    struct trace *t_prev, *t_result;

    while(1)
    {
        written = __atomic_load_n(&ts->num_traces_written, __ATOMIC_ACQUIRE);
        if(index < written)
        {
            debug("Index %li < written %li, exiting\n", index, ts->num_traces_written);
            break;
        }

        prev_index = __atomic_fetch_add(&ts->prev_next_trace,
                                        1, __ATOMIC_RELAXED);

        debug("Checking prev_index %li\n", prev_index);
        if(prev_index >= ts_num_traces(ts->prev))
        {
            err("Index %li out of bounds for previous trace set\n", prev_index);
            return -EINVAL;
        }

        ret = __list_create_entry(queue, &node, prev_index);
        if(ret < 0)
        {
            err("Failed to add prev index to synchronization list\n");
            return ret;
        }

        ret = trace_get(ts->prev, &t_prev, prev_index, true);
        if(ret < 0)
        {
            err("Failed to get trace from previous trace set\n");
            return ret;
        }

        if(!t_prev->buffered_samples ||
           (ts->prev->title_size != 0 && !t_prev->buffered_title) ||
           (ts->prev->data_size != 0 && !t_prev->buffered_data))
        {
            debug("prev_index %li not a valid index\n", prev_index);
            trace_free(t_prev);
            __list_remove_entry(queue, node);
            continue;
        }

        debug("prev_index %li is a valid index, appending\n", prev_index);
        ret = __create_new_trace_from_prev(&t_result, t_prev);
        t_result->owner = ts;

        trace_free(t_prev);
        if(ret < 0)
        {
            err("Failed to create new trace from previous\n");
            return ret;
        }

        entry = list_get_data(node);
        entry->trace = t_result;

        ret = sem_post(&queue->event);
        if(ret < 0)
        {
            err("Failed to post to commit thread event signal\n");
            ret = -errno;
            goto __fail_free_result;
        }
    }

    return 0;

__fail_free_result:
    trace_free_memory(t_result);
    return ret;
}

int __tfm_save_title(struct trace *t, char **title)
{
    int ret;
    if(TRACE_IDX(t) >= t->owner->num_traces_written)
    {
        ret = __render_to_index(t->owner, TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to render trace set to index %li\n", TRACE_IDX(t));
            return ret;
        }
    }

    ret = __read_title_from_file(t, title);
    if(ret < 0)
    {
        err("Failed to read title from file\n");
        return ret;
    }

    return 0;
}

int __tfm_save_data(struct trace *t, uint8_t **data)
{
    int ret;
    if(TRACE_IDX(t) >= t->owner->num_traces_written)
    {
        ret = __render_to_index(t->owner, TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to render trace set to index %li\n", TRACE_IDX(t));
            return ret;
        }
    }

    ret = __read_data_from_file(t, data);
    if(ret < 0)
    {
        err("Failed to read data from file\n");
        return ret;
    }

    return 0;
}

int __tfm_save_samples(struct trace *t, float **samples)
{
    int ret;
    if(TRACE_IDX(t) >= t->owner->num_traces_written)
    {
        ret = __render_to_index(t->owner, TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to render trace set to index %li\n", TRACE_IDX(t));
            return ret;
        }
    }

    ret = __read_samples_from_file(t, samples);
    if(ret < 0)
    {
        err("Failed to read samples from file\n");
        return ret;
    }

    return 0;
}

void __tfm_save_exit(struct trace_set *ts)
{
    // todo finalize headers
    // todo close file
}

void __tfm_save_free_title(struct trace *t)
{
    passthrough_free_title(t);
}

void __tfm_save_free_data(struct trace *t)
{
    passthrough_free_data(t);
}

void __tfm_save_free_samples(struct trace *t)
{
    passthrough_free_samples(t);
}

int tfm_save(struct tfm **tfm, char *path_prefix)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_save);
    res->tfm_data = NULL;

    res->tfm_data = calloc(1, sizeof(struct tfm_save));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->prefix = path_prefix;
    *tfm = res;
    return 0;
}
