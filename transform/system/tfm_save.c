#include "transform.h"
#include "trace.h"
#include "list.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <immintrin.h>

#define TFM_DATA(tfm)   ((struct tfm_save *) (tfm)->tfm_data)
#define SENTINEL        SIZE_MAX

struct tfm_save
{
    char *prefix;
};

struct tfm_save_data
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
    pthread_t handle;
    sem_t event;
    sem_t sentinel;

    struct trace_set *ts;
    int thread_ret;

    sem_t list_lock;
    struct list_head head;
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
    fprintf(stderr, " %li (%p)\n", entry->prev_index, entry->trace);
}

int __list_create_entry(struct __commit_queue *queue,
                        struct __commit_queue_entry **res,
                        size_t prev_index)
{
    int ret;
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

    ret = sem_wait(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on commit queue lock\n");
        return -errno;
    }

    list_for_each_entry(curr, &queue->head, list)
    {
        if(curr->prev_index > prev_index)
            break;
    }
    list_add_tail(&entry->list, &curr->list);

    ret = sem_post(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to post to commit queue lock\n");
        return -errno;
    }

    *res = entry;
    return 0;
}

int __list_remove_entry(struct __commit_queue *queue,
                        struct __commit_queue_entry *entry)
{
    int ret;
    if(!queue || !entry)
    {
        err("Invalid commit queue or node\n");
        return -EINVAL;
    }

    ret = sem_wait(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to wait on commit queue lock\n");
        return -errno;
    }

    list_del(&entry->list);
    free(entry);

    ret = sem_post(&queue->list_lock);
    if(ret < 0)
    {
        err("Failed to post to commit queue lock\n");
        return -errno;
    }

    return 0;
}

int __append_trace_sequential(struct trace *t)
{
    int i, j;
    int ret;
    size_t written;

    size_t temp_len;
    void *temp = NULL;
    float curr_batch[8];
    __m256 curr, scale;

    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    scale = _mm256_broadcast_ss(&t->owner->yscale);
    switch(t->owner->datatype)
    {
        case DT_BYTE:
            temp_len = ts_num_samples(t->owner) * sizeof(char);
            temp = calloc(t->owner->num_samples, sizeof(char));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner);)
            {
                if(i + 8 < ts_num_samples(t->owner))
                {
                    curr = _mm256_loadu_ps(&t->buffered_samples[i]);
                    curr = _mm256_div_ps(curr, scale);
                    _mm256_storeu_ps(curr_batch, curr);

                    for(j = 0; j < 8; j++)
                        ((char *) temp)[i + j] = (char) curr_batch[j];

                    i += 8;
                }
                else
                {
                    ((char *) temp)[i] = (char) (t->buffered_samples[i] / t->owner->yscale);
                    i++;
                }
            }

            break;

        case DT_SHORT:
            temp_len = ts_num_samples(t->owner) * sizeof(short);
            temp = calloc(t->owner->num_samples, sizeof(short));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner);)
            {
                if(i + 8 < ts_num_samples(t->owner))
                {
                    curr = _mm256_loadu_ps(&t->buffered_samples[i]);
                    curr = _mm256_div_ps(curr, scale);
                    _mm256_storeu_ps(curr_batch, curr);

                    for(j = 0; j < 8; j++)
                        ((short *) temp)[i + j] = (short) curr_batch[j];

                    i += 8;
                }
                else
                {
                    ((short *) temp)[i] = (short) (t->buffered_samples[i] / t->owner->yscale);
                    i++;
                }
            }

            break;

        case DT_INT:
            temp_len = ts_num_samples(t->owner) * sizeof(int);
            temp = calloc(t->owner->num_samples, sizeof(int));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner);)
            {
                if(i + 8 < ts_num_samples(t->owner))
                {
                    curr = _mm256_loadu_ps(&t->buffered_samples[i]);
                    curr = _mm256_div_ps(curr, scale);
                    _mm256_storeu_ps(curr_batch, curr);

                    for(j = 0; j < 8; j++)
                        ((int *) temp)[i + j] = (int) curr_batch[j];

                    i += 8;
                }
                else
                {
                    ((int *) temp)[i] = (int) (t->buffered_samples[i] / t->owner->yscale);
                    i++;
                }
            }

            break;

        case DT_FLOAT:
            temp_len = ts_num_samples(t->owner) * sizeof(float);
            temp = calloc(t->owner->num_samples, sizeof(float));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner);)
            {
                if(i + 8 < ts_num_samples(t->owner))
                {
                    curr = _mm256_loadu_ps(&t->buffered_samples[i]);
                    curr = _mm256_div_ps(curr, scale);
                    _mm256_storeu_ps(curr_batch, curr);

                    for(j = 0; j < 8; j++)
                        ((float *) temp)[i + j] = curr_batch[j];

                    i += 8;
                }
                else
                {
                    ((float *) temp)[i] = (t->buffered_samples[i] / t->owner->yscale);
                    i++;
                }
            }

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
        debug("Trace %li writing %li bytes of title\n", TRACE_IDX(t), t->owner->title_size);
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
        debug("Trace %li writing %li bytes of data\n", TRACE_IDX(t), t->owner->data_size);
        written = fwrite(t->buffered_data, 1, t->owner->data_size, t->owner->ts_file);
        if(written != t->owner->data_size)
        {
            err("Failed to write all bytes of data to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    debug("Trace %li writing %li bytes of samples\n", TRACE_IDX(t), temp_len);
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

void *__commit_thread(void *arg)
{
    int ret, count;
    size_t written = 0, prev_index;
    bool sentinel_seen = false;

    struct __commit_queue *queue = arg;
    struct __commit_queue_entry *entry, *curr;
    struct tfm_save_data *tfm_data = queue->ts->tfm_data;

    struct trace *trace_to_commit;
    LIST_HEAD(write_head);

    while(1)
    {
        ret = sem_wait(&queue->event);
        if(ret < 0)
        {
            err("Commit thread failed to wait on event signal\n");
            queue->thread_ret = -errno;
            pthread_exit(NULL);
        }

        if(queue->ts == NULL)
        {
            debug("Commit thread exiting cleanly\n");
            queue->thread_ret = 0;
            pthread_exit(NULL);
        }

        // examine the commit list
        ret = sem_wait(&queue->list_lock);
        if(ret < 0)
        {
            err("Commit thread failed to wait on list lock\n");
            queue->thread_ret = -errno;
            pthread_exit(NULL);
        }

        count = 0;
        list_for_each_entry(curr, &queue->head, list)
        {
            if(curr->trace || curr->prev_index == SENTINEL)
                count++;
            else break;
        }
        list_cut_before(&write_head, &queue->head, &curr->list);

        ret = sem_post(&queue->list_lock);
        if(ret < 0)
        {
            err("Commit thread failed to post to list lock\n");
            queue->thread_ret = -errno;
            pthread_exit(NULL);
        }

        // write the collected batch
        if(!list_empty(&write_head))
        {
            debug("Committing %li -> %li (%i)\n", written, written + count, count);

            ret = sem_wait(&queue->ts->file_lock);
            if(ret < 0)
            {
                err("Failed to wait on trace set file lock\n");
                queue->thread_ret = -errno;
                pthread_exit(NULL);
            }

            // initial fseek, all further writes are sequential
            entry = list_entry(write_head.next, struct __commit_queue_entry, list);
            if(entry->trace)
            {
                entry->trace->start_offset = queue->ts->trace_start + written * queue->ts->trace_length;
                ret = fseek(entry->trace->owner->ts_file, entry->trace->start_offset, SEEK_SET);
                if(ret)
                {
                    err("Failed to seek to trace set file position\n");
                    queue->thread_ret = ret;
                    pthread_exit(NULL);
                }
            }

            while(!list_empty(&write_head))
            {
                entry = list_entry(write_head.next, struct __commit_queue_entry, list);

                prev_index = entry->prev_index;
                trace_to_commit = entry->trace;

                list_del(&entry->list);
                free(entry);

                if(prev_index == SENTINEL)
                {
                    debug("Encountered sentinel, setting num_traces %li\n",
                          written);

                    sentinel_seen = true;
                    __atomic_store(&queue->ts->num_traces,
                                   &written, __ATOMIC_RELEASE);

                    ret = sem_post(&queue->sentinel);
                    if(ret < 0)
                    {
                        err("Failed to post to sentinel signal\n");
                        queue->thread_ret = -errno;
                        pthread_exit(NULL);
                    }
                }
                else
                {
                    if(!sentinel_seen)
                    {
                        if(trace_to_commit->owner != queue->ts)
                        {
                            err("Bad trace to commit -- unknown trace set\n");
                            queue->thread_ret = -EINVAL;
                            pthread_exit(NULL);
                        }

                        trace_to_commit->start_offset = queue->ts->trace_start + written * queue->ts->trace_length;
                        ret = __append_trace_sequential(trace_to_commit);
                        if(ret < 0)
                        {
                            err("Failed to append trace to file\n");
                            queue->thread_ret = ret;
                            pthread_exit(NULL);
                        }

                        trace_free_memory(trace_to_commit);
                        written++;
                    }
                    else
                    {
                        err("Encountered trace to write after seeing sentinel\n");
                        queue->thread_ret = -EINVAL;
                        pthread_exit(NULL);
                    }
                }
            }

            // todo: sometimes crash here when using tfm_wait and debugging?
            // update global written counter
            __atomic_store(&tfm_data->num_traces_written, &written, __ATOMIC_RELEASE);

            ret = sem_post(&queue->ts->file_lock);
            if(ret < 0)
            {
                err("Failed to post to trace set file lock\n");
                queue->thread_ret = -errno;
                pthread_exit(NULL);
            }
        }
    }
}

int __tfm_save_init(struct trace_set *ts)
{
    int ret;
    char fname[256];
    struct __commit_queue *queue;
    struct tfm_save_data *tfm_data;

    struct tfm_save *tfm = TFM_DATA(ts->tfm);

    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = -2; // header.c detects this as invalid

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

    tfm_data = calloc(1, sizeof(struct tfm_save_data));
    if(!tfm_data)
    {
        err("Failed to allocate transformation data\n");
        return -ENOMEM;
    }

    tfm_data->num_traces_written = 0;
    tfm_data->prev_next_trace = 0;

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

    queue->ts = ts;
    queue->thread_ret = 0;
    LIST_HEAD_INIT_INLINE(queue->head);

    ret = sem_init(&queue->event, 0, 0);
    if(ret < 0)
    {
        err("Failed to initialize queue event semaphore\n");
        goto __free_commit_queue;
    }

    ret = sem_init(&queue->sentinel, 0, 0);
    if(ret < 0)
    {
        err("Failed to initialize sentinel semaphore\n");
        goto __destroy_queue_event;
    }

    ret = sem_init(&queue->list_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize queue list lock\n");
        goto __destroy_sentinel_event;
    }

    ret = pthread_create(&queue->handle, NULL,
                         __commit_thread, queue);
    if(ret < 0)
    {
        err("Failed to create commit thread\n");
        goto __destroy_queue_list;
    }

    tfm_data->commit_data = queue;
    ts->tfm_data = tfm_data;
    return 0;

__destroy_queue_list:
    sem_destroy(&queue->list_lock);

__destroy_sentinel_event:
    sem_destroy(&queue->sentinel);

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

int __tfm_save_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_save_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __render_to_index(struct trace_set *ts, size_t index)
{
    int ret;
    size_t written, prev_index;

    struct tfm_save_data *tfm_data = ts->tfm_data;
    struct __commit_queue *queue = tfm_data->commit_data;
    struct __commit_queue_entry *entry;
    struct trace *t_prev, *t_result;

    while(1)
    {
        written = __atomic_load_n(&tfm_data->num_traces_written, __ATOMIC_ACQUIRE);
        if(index < written)
        {
            debug("Index %li < written %li, exiting\n", index, tfm_data->num_traces_written);
            break;
        }

        prev_index = __atomic_fetch_add(&tfm_data->prev_next_trace,
                                        1, __ATOMIC_RELAXED);

        debug("Checking prev_index %li\n", prev_index);
        // todo this might be the bad condition for chained tfm_saves
        if(prev_index >= ts_num_traces(ts->prev))
        {
            // send a sentinel down the pipeline
            debug("Index %li out of bounds for previous trace set\n", prev_index);

            ret = __list_create_entry(queue, &entry, SENTINEL);
            if(ret < 0)
            {
                err("Failed to add sentinel to synchronization list\n");
                return ret;
            }

            ret = sem_post(&queue->event);
            if(ret < 0)
            {
                err("Failed to post to commit thread event signal\n");
                ret = -errno;
                goto __fail_free_result;
            }

            return 1;
        }

        ret = __list_create_entry(queue, &entry, prev_index);
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
            __list_remove_entry(queue, entry);
            continue;
        }

        debug("prev_index %li is a valid index, appending\n", prev_index);
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

typedef enum
{
    KIND_TITLE = 0,
    KIND_DATA,
    KIND_SAMPLES
} tfm_save_kind_t;

const char *kind_str[] = {
        [KIND_TITLE] = "title",
        [KIND_DATA] = "data",
        [KIND_SAMPLES] = "samples"
};

int __tfm_save_generic(struct trace *t, void **out, tfm_save_kind_t kind)
{
    int ret;
    struct tfm_save_data *tfm_data = t->owner->tfm_data;
    size_t written = __atomic_load_n(&tfm_data->num_traces_written, __ATOMIC_ACQUIRE);
    struct __commit_queue *queue = tfm_data->commit_data;

    if(kind != KIND_TITLE && kind != KIND_DATA && kind != KIND_SAMPLES)
    {
        err("Invalid generic kind\n");
        return -EINVAL;
    }

    debug("%s for trace %li, written = %li\n", kind_str[kind], TRACE_IDX(t), written);
    if(TRACE_IDX(t) >= written)
    {
        ret = __render_to_index(t->owner, TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to render trace set to index %li\n", TRACE_IDX(t));
            return ret;
        }
        else if(ret == 1)
        {
            ret = sem_wait(&queue->sentinel);
            if(ret < 0)
            {
                err("Failed to wait on sentinel signal\n");
                return -errno;
            }
        }
    }

    if(TRACE_IDX(t) < t->owner->num_traces)
    {
        switch(kind)
        {
            case KIND_TITLE:
                ret = read_title_from_file(t, (char **) out);
                break;

            case KIND_DATA:
                ret = read_data_from_file(t, (uint8_t **) out);
                break;

            case KIND_SAMPLES:
                ret = read_samples_from_file(t, (float **) out);
                break;

            default:
                err("Invalid generic kind\n");
                ret = -EINVAL;
                break;
        }

        if(ret < 0)
        {
            err("Failed to read %s from file for trace %li\n", kind_str[kind], TRACE_IDX(t));
            return ret;
        }
    }
    else
    {
        debug("Setting trace %li %s to null\n", TRACE_IDX(t), kind_str[kind]);
        *out = NULL;
    }
    return 0;
}

int __tfm_save_title(struct trace *t, char **title)
{
    return __tfm_save_generic(t, (void **) title, KIND_TITLE);
}

int __tfm_save_data(struct trace *t, uint8_t **data)
{
    return __tfm_save_generic(t, (void **) data, KIND_DATA);
}

int __tfm_save_samples(struct trace *t, float **samples)
{
    return __tfm_save_generic(t, (void **) samples, KIND_SAMPLES);
}

void __tfm_save_exit(struct trace_set *ts)
{
    int ret;
    struct tfm_save_data *tfm_data = ts->tfm_data;
    struct __commit_queue *queue = tfm_data->commit_data;

    // first, wait for commit list to drain
    while(1)
    {
        ret = sem_wait(&queue->list_lock);
        if(ret < 0)
        {
            err("Failed to wait on commit queue list lock\n");
            return;
        }

        if(list_empty(&queue->head))
        {
            debug("Commit queue is drained\n");
            break;
        }

        ret = sem_post(&queue->list_lock);
        if(ret < 0)
        {
            err("Failed to post to commit queue list lock\n");
            return;
        }
    }

    // kill the commit thread
    queue->ts = NULL;
    sem_post(&queue->event);
    pthread_join(queue->handle, NULL);

    sem_destroy(&queue->list_lock);
    sem_destroy(&queue->event);
    free(queue);

    // finalize headers
    ts->num_traces = tfm_data->num_traces_written;

    ret = finalize_headers(ts);
    if(ret < 0)
    {
        err("Failed to finalize headers\n");
        return;
    }

    // clean up file
    sem_destroy(&ts->file_lock);
    fclose(ts->ts_file);
    ts->ts_file = NULL;

    free(tfm_data);
    ts->tfm_data = NULL;
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
