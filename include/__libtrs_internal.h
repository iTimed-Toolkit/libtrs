#ifndef LIBTRS___LIBTRS_INTERNAL_H
#define LIBTRS___LIBTRS_INTERNAL_H

#include <stdio.h>
#include <stdint.h>
#include <semaphore.h>

struct list;

struct trace_set
{
    // for reading from files
    FILE *ts_file;
    sem_t file_lock;

    // size params
    size_t set_id;
    size_t num_samples, num_traces;

    // for new trace sets
    size_t num_traces_written;
    size_t prev_next_trace;
    void *commit_data;

    // commonly used, buffered headers
    size_t title_size, data_size;
    size_t input_offs, input_len,
            output_offs, output_len,
            key_offs, key_len;
    enum datatype
    {
        DT_BYTE = 0x1, DT_SHORT = 0x2,
        DT_INT = 0x4, DT_FLOAT = 0x14,
        DT_NONE = 0xFF
    } datatype;
    float yscale;

    size_t trace_start, trace_length;

    size_t num_headers;
    struct th_data *headers;

    // for transformations
    struct trace_set *prev;
    struct tfm *tfm;

    struct trace_cache *cache;
};

struct trace
{
    struct trace_set *owner;
    size_t start_offset;

    char *buffered_title;
    uint8_t *buffered_data;
    float *buffered_samples;
};

typedef enum
{
    DEBUG = 0,
    WARN,
    ERR,
    CRITICAL
} log_level_t;

static log_level_t libtrs_log_level = ERR;

#define __first_arg(a, ...)     a
#define __other_arg(a, ...)     , ## __VA_ARGS__
#define __msg(level, ...)       if(level >= libtrs_log_level) {                 \
                                     fprintf(stderr, "libtrs >>> %s @ %i: "     \
                                        __first_arg(__VA_ARGS__), __FUNCTION__, \
                                        __LINE__ __other_arg(__VA_ARGS__));     \
                                        fflush(stderr); }

#define debug(...)              __msg(DEBUG, __VA_ARGS__)
#define warn(...)               __msg(WARN, __VA_ARGS__)
#define err(...)                __msg(ERR, __VA_ARGS__)
#define critical(...)           __msg(CRITICAL, __VA_ARGS__)

#define TRACE_IDX(t)  (((t)->start_offset - (t)->owner->trace_start)  / \
                            (t)->owner->trace_length)

int trace_free_memory(struct trace *t);
int __read_title_from_file(struct trace *t, char **title);
int __read_samples_from_file(struct trace *t, float **samples);
int __read_data_from_file(struct trace *t, uint8_t **data);

int read_headers(struct trace_set *ts);
int write_default_headers(struct trace_set *ts);
int write_inherited_headers(struct trace_set *ts);
int finalize_headers(struct trace_set *ts);
int free_headers(struct trace_set *ts);

int tc_lookup(struct trace_set *ts, size_t index, struct trace **trace);
int tc_store(struct trace_set *ts, size_t index, struct trace *trace);
int tc_deref(struct trace_set *ts, size_t index, struct trace *trace);
int tc_free(struct trace_set *ts);

typedef int (*list_comparison_t)(void *node1, void *node2);
typedef void (*list_print_t)(void *data);

int list_create_node(struct list **node, void *data);
int list_free_node(struct list *node);

/**
 * list_comparison_t(void *, void *): compare the data from two
 *      different list nodes for ordering purposes. Return positive
 *      if node1 should go before node2, negative if node1 should go
 *      after node2, or 0 if don't care
 */
int list_link_single(struct list **head, struct list *node, list_comparison_t f);
int list_unlink_single(struct list **head, struct list *node);
int list_lookup_single(struct list *head, struct list *node);
void *list_get_data(struct list *node);
int list_dump(struct list *head, list_print_t f);

#endif //LIBTRS___LIBTRS_INTERNAL_H
