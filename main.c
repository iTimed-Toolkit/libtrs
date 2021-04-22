#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>

#include "trace.h"
#include "__trace_internal.h"

#include "transform.h"
#include "list.h"

#define MAX_LINELENGTH      512
#define MAX_TFM_DEPTH       64
#define SEPARATORS          " \n"

#define STR_AT_IDX(s)       [s] = (#s)

static const char *port_t_strings[] = {
        STR_AT_IDX(PORT_ECHO),
        STR_AT_IDX(PORT_CPA_PROGRESS),
        STR_AT_IDX(PORT_CPA_SPLIT_PM),
        STR_AT_IDX(PORT_CPA_SPLIT_PM_PROGRESS)
};

static const char *fill_order_t_strings[] = {
        STR_AT_IDX(ROWS),
        STR_AT_IDX(COLS),
        STR_AT_IDX(PLOTS)
};

static const char *block_t_strings[] = {
        STR_AT_IDX(BLOCK_MAX),
        STR_AT_IDX(BLOCK_MIN),
        STR_AT_IDX(BLOCK_MAXABS),
        STR_AT_IDX(BLOCK_MINABS)
};

static const char *aes_leakage_t_strings[] = {
        STR_AT_IDX(AES128_R0_R1_HD_NOMC),
        STR_AT_IDX(AES128_R0_HW_SBOXOUT),
        STR_AT_IDX(AES128_R10_OUT_HD),
        STR_AT_IDX(AES128_R10_HW_SBOXIN)
};

#define NUM_TABLE_ENTRIES(table)        (sizeof(table) / (sizeof((table)[0])))
#define IF_NEXT(c, dothis)              if(*(c)) { dothis; }
#define IF_NOT_NEXT(c, dothis)          if(!(*(c))) { dothis; }
#define CHECK_NEXT(c, fail)             IF_NOT_NEXT(c, err("CHECK_NEXT FAILED\n"); fail)

int __parse_string(char **config, char **res)
{
    strsep(config, "\"");
    if(!(*config))
    {
        err("Failed to parse opening quote for string\n");
        return -EINVAL;
    }

    *res = strsep(config, "\"");
    debug("Got string %s\n", *res);

    if(!(*config))
    {
        err("Failed to parse closing quote for string\n");
        return -EINVAL;
    }

    return 0;
}

int __parse__Bool(char **config, bool *res)
{
    char *tok = strsep(config, SEPARATORS);

    if(strcmp(tok, "true") == 0)
        *res = true;
    else if(strcmp(tok, "false") == 0)
        *res = false;
    else
    {
        err("Failed to parse boolean\n");
        return -EINVAL;
    }

    return 0;
}

int __parse_int(char **config, int *res)
{
    char *tok = strsep(config, SEPARATORS);
    *res = (int) strtol(tok, NULL, 10);
    return 0;
}

int __parse_size_t(char **config, size_t *res)
{
    char *tok = strsep(config, SEPARATORS);
    *res = strtol(tok, NULL, 10);
    return 0;
}

int __parse_double(char **config, double *res)
{
    char *tok = strsep(config, SEPARATORS);
    *res = strtod(tok, NULL);
    return 0;
}

int __parse_memsize(char **config, size_t *size)
{
    char *tok = strsep(config, SEPARATORS);
    int pos, unit, mul = 1;
    size_t res = 0ull;

    pos = 0;
    while(tok[pos])
    {
        if(tok[pos] >= '0' && tok[pos] <= '9')
            pos++;
        else break;
    }

    unit = pos;

    pos--;
    while(pos >= 0)
    {
        res += (tok[pos] - '0') * mul;
        mul *= 10;
        pos--;
    }

    if(tok[unit] == 'G')
        res *= (1024 * 1024 * 1024);
    else if(tok[unit] == 'M')
        res *= (1024 * 1024);
    else if(tok[unit] == 'K')
        res *= 1024;
    else if(tok[unit] != 'B')
    {
        err("Invalid size specifier\n");
        return -EINVAL;
    }

    *size = res;
    return 0;
}

int __parse_enum(char **config, int *res, const char *enum_table[], int entries)
{
    int i;
    char *tok = strsep(config, SEPARATORS);

    for(i = 0; i < entries; i++)
    {
        if(strcmp(tok, enum_table[i]) == 0)
        {
            *res = i;
            return 0;
        }
    }

    err("No matching enum found in table\n");
    return -EINVAL;
}

#define string_tt       char *
#define bool_tt         bool
#define int_tt          int
#define memsize_tt      size_t
#define enum_tt         int
#define size_t_tt       size_t
#define double_tt       double

#define init_string     NULL
#define init_bool       false
#define init_int        (-1)
#define init_memsize    (-1)
#define init_enum       (-1)
#define init_size_t     (-1)
#define init_double     (-1.0)

#define __parse_arg_nodecl(name, type, c)           \
    CHECK_NEXT(c, return -EINVAL)                   \
    ret = __parse_ ## type (c, &(name));            \
    if(ret < 0) {                                   \
        err("Failed to parse " #name " " #type "\n");    \
        return ret; }

#define __parse_enum_nodecl(name, table, c)         \
    CHECK_NEXT(c, return -EINVAL);                  \
    ret = __parse_enum(c, &(name), table,           \
                        NUM_TABLE_ENTRIES(table));  \
    if(ret < 0) {                                   \
        err("Failed to parse " #name " enum\n");      \
        return ret; }

#define parse_arg(name, type, c)                    \
    type ## _tt name = init_ ## type;               \
    __parse_arg_nodecl(name, type, c)

#define parse_enum(name, table, c)                  \
    int name = init_enum;                           \
    __parse_enum_nodecl(name, table, c)

#define parse_arg_optional(name, type, c)           \
    type ## _tt name = init_ ## type;               \
    IF_NEXT(c, __parse_arg_nodecl(name, type, c))

#define PARSE_FUNC(tfm_name, exec, ...)             \
    int __parse_ ## tfm_name (char **config,        \
                            struct tfm **tfm) {     \
    int ret; exec; ret = tfm_name(tfm, __VA_ARGS__);\
    if(ret < 0) {                                   \
        err("Failed to create " #tfm_name  "\n");   \
        return ret;} return 0; }


int __parse_tfm_source(char **config, struct trace_set **ts)
{
    int ret;
    parse_arg(fname, string, config);

    ret = ts_open(ts, fname);
    if(ret < 0)
    {
        err("Failed to open trace set %s\n", fname);
        return ret;
    }

    return 0;
}

PARSE_FUNC(tfm_save,
           parse_arg(path, string, config),
           path);

PARSE_FUNC(tfm_wait_on,
           parse_enum(port, port_t_strings, config);
                   parse_arg(bufsize, memsize, config),
           port, bufsize)

PARSE_FUNC(tfm_visualize,
           struct viz_args args = {.filename = NULL};
                   __parse_arg_nodecl(args.rows, int, config);
                   __parse_arg_nodecl(args.cols, int, config);
                   __parse_arg_nodecl(args.plots, int, config);
                   __parse_arg_nodecl(args.samples, int, config);

                   for(int i = 0; i < 3; i++)
           {
                   __parse_enum_nodecl(args.order[i], fill_order_t_strings, config);
           }

           // optionally get filename
           IF_NEXT(config, if(*(*config) != '(') { __parse_arg_nodecl(args.filename, string, config))},
           &args)

PARSE_FUNC(tfm_average,
           parse_arg(per_sample, bool, config),
           per_sample)

PARSE_FUNC(tfm_block_select,
           parse_arg(blocksize, int, config);
                   parse_enum(block, block_t_strings, config),
           blocksize, block)

PARSE_FUNC(tfm_split_tvla,
           parse_arg(which, bool, config),
           which)

PARSE_FUNC(tfm_narrow,
           parse_arg(first_trace, int, config);
                   parse_arg(num_traces, int, config);
                   parse_arg(first_sample, int, config);
                   parse_arg(num_samples, int, config),
           first_trace, num_traces, first_sample, num_samples)

PARSE_FUNC(tfm_static_align,
           parse_arg(confidence, double, config);
                   parse_arg(max_shift, int, config);
                   parse_arg(ref_trace, size_t, config);
                   parse_arg(lower, int, config);
                   parse_arg(upper, int, config),
           confidence, max_shift, ref_trace, 1, &lower, &upper);

PARSE_FUNC(tfm_io_correlation,
           parse_arg(granularity, int, config);
                   parse_arg(num, int, config),
           granularity, num)

PARSE_FUNC(tfm_aes_intermediate,
           parse_arg(verify_data, bool, config);
                   parse_enum(model, aes_leakage_t_strings, config),
           verify_data, model)

struct async_render_entry
{
    struct list_head list;
    struct render *render;
};

struct trace_set_entry
{
    struct list_head list;
    struct trace_set *set;
};

struct parse_args
{
    struct list_head async_renders;
    struct list_head trace_sets;

    struct trace_set *main_render;
    size_t main_nthreads;
};

int parse_cache(char **config, struct trace_set *ts)
{
    int ret;
    parse_arg(size, memsize, config);
    parse_arg(assoc, size_t, config);

    ret = ts_create_cache(ts, size, assoc);
    if(ret < 0)
    {
        err("Failed to create cache\n");
        return ret;
    }

    return 0;
}

int parse_render(char **config, struct trace_set *ts, struct parse_args *parsed)
{
    int ret;
    parse_arg(nthreads, size_t, config);

    parsed->main_render = ts;
    parsed->main_nthreads = nthreads;
    return 0;
}

int parse_render_async(char **config, struct trace_set *ts, struct parse_args *parsed)
{
    int ret;
    struct async_render_entry *entry;

    parse_arg(nthreads, size_t, config);

    entry = calloc(1, sizeof(struct async_render_entry));
    if(!entry)
    {
        err("Failed to allocate async render entry\n");
        return -ENOMEM;
    }

    ret = ts_render_async(ts, nthreads, &entry->render);
    if(ret < 0)
    {
        err("Failed to create async render\n");
        return ret;
    }

    list_add(&entry->list, &parsed->async_renders);
    return 0;
}

int parse_extras(char **config, struct trace_set *ts, struct parse_args *parsed)
{
    int ret;
    char *type;

    strsep(config, "(");
    while(1)
    {
        type = strsep(config, SEPARATORS);
        IF_NOT_NEXT(config, break);

        if(strcmp(type, "cache") == 0)
        {
            ret = parse_cache(config, ts);
            if(ret < 0)
            {
                err("Failed to parse cache\n");
                return ret;
            }
        }
        else if(strcmp(type, "render") == 0)
        {
            if(parsed->main_render)
            {
                err("Duplicate main renders not supported\n");
                return -EINVAL;
            }
            else
            {
                ret = parse_render(config, ts, parsed);
                if(ret < 0)
                {
                    err("Failed to parse main render\n");
                    return ret;
                }
            }
        }
        else if(strcmp(type, "render_async") == 0)
        {
            ret = parse_render_async(config, ts, parsed);
            if(ret < 0)
            {
                err("Failed to parse async render\n");
                return ret;
            }
        }
        else
        {
            err("Invalid extra argument: %s\n", type);
            return -EINVAL;
        }
    }
}

int parse_transform(char *line, struct trace_set **ts,
                    struct trace_set *prev, struct parse_args *parsed)
{
    int ret;
    char *curr = line, *type;

    struct trace_set_entry *new_entry;
    struct tfm *tfm = NULL;

    type = strsep(&curr, SEPARATORS);

    // system
    if(strcmp(type, "source") == 0)
        ret = __parse_tfm_source(&curr, ts);
    else if(strcmp(type, "save") == 0)
        ret = __parse_tfm_save(&curr, &tfm);
    else if(strcmp(type, "wait_on") == 0)
        ret = __parse_tfm_wait_on(&curr, &tfm);
    else if(strcmp(type, "visualize") == 0)
        ret = __parse_tfm_visualize(&curr, &tfm);

        // analysis
    else if(strcmp(type, "average") == 0)
        ret = __parse_tfm_average(&curr, &tfm);
    else if(strcmp(type, "block_select") == 0)
        ret = __parse_tfm_block_select(&curr, &tfm);

        // traces
    else if(strcmp(type, "split_tvla") == 0)
        ret = __parse_tfm_split_tvla(&curr, &tfm);
    else if(strcmp(type, "narrow") == 0)
        ret = __parse_tfm_narrow(&curr, &tfm);

        // alignment
    else if(strcmp(type, "static_align") == 0)
        ret = __parse_tfm_static_align(&curr, &tfm);

        // correlation
    else if(strcmp(type, "io_correlation") == 0)
        ret = __parse_tfm_io_correlation(&curr, &tfm);
    else if(strcmp(type, "aes_intermediate") == 0)
        ret = __parse_tfm_aes_intermediate(&curr, &tfm);

    else
    {
        err("Unknown transform: %s\n", type);
        return -EINVAL;
    }

    if(ret < 0)
    {
        err("Failed to get tfm from parser\n");
        return ret;
    }

    if(tfm)
    {
        ret = ts_transform(ts, prev, tfm);
        if(ret < 0)
        {
            err("Failed to link trace sets\n");
            return ret;
        }
    }

    IF_NEXT(&curr,
            ret = parse_extras(&curr, *ts, parsed);
            if(ret < 0)
            {
                err("Failed to parse extras\n");
                return ret;
            }
    )

    new_entry = calloc(1, sizeof(struct trace_set_entry));
    if(!new_entry)
    {
        err("Failed to allocate new trace set entry\n");
        return -ENOMEM;
    }

    new_entry->set = *ts;
    list_add(&new_entry->list, &parsed->trace_sets);

    return 0;
}

int parse_config(char *fname, struct parse_args *parsed)
{
    int ret, count = 0;
    char line[MAX_LINELENGTH];

    int last_depth = 0, depth, nspace, pos;
    struct trace_set *nodes[MAX_TFM_DEPTH];

    FILE *config = fopen(fname, "r");
    if(!config)
    {
        err("Failed to open config file\n");
        return -EINVAL;
    }

    while(1)
    {
        if(fgets(line, MAX_LINELENGTH, config))
        {
            count++;
            depth = nspace = pos = 0;

            while(line[pos] == '\t' || line[pos] == ' ')
            {
                if(line[pos] == '\t')
                    depth++;
                else
                {
                    nspace++;
                    if(nspace % 4 == 0)
                    {
                        depth++;
                        nspace = 0;
                    }
                }

                pos++;
            }

            if(nspace != 0)
            {
                err("Invalid spacing detected: not multiple of 4 spaces\n");
                ret = -EINVAL;
                goto out;
            }

            if(depth >= MAX_TFM_DEPTH)
            {
                err("Configuration nested too deeply\n");
                ret = -EINVAL;
                goto out;
            }

            if(depth == last_depth || depth == last_depth + 1 || depth < last_depth)
            {
                ret = parse_transform(&line[pos], &nodes[depth],
                                      (depth == 0) ? NULL : nodes[depth - 1],
                                      parsed);
                if(ret < 0)
                {
                    err("Failed to parse transform on line %i\n", count);
                    goto out;
                }

                last_depth = depth;
            }
        }
        else break;
    }

    ret = 0;
out:
    fclose(config);
    return ret;
}

int main(int argc, char *argv[])
{
    int ret;

    struct async_render_entry *curr_render, *n_render;
    struct trace_set_entry *curr_ts, *n_ts;

    struct parse_args parsed;
    LIST_HEAD_INIT_INLINE(parsed.async_renders);
    LIST_HEAD_INIT_INLINE(parsed.trace_sets);

    parsed.main_render = NULL;

    if(argc != 2)
    {
        err("Usage: libtrace_evaluate [cfg filename]\n");
        return -EINVAL;
    }

    ret = parse_config(argv[1], &parsed);
    if(ret < 0)
    {
        err("Failed to parse config\n");
        return ret;
    }

    ret = ts_render(parsed.main_render, parsed.main_nthreads);
    if(ret < 0)
    {
        err("Failed to render main trace set\n");
        return ret;
    }

    list_for_each_entry_safe(curr_render, n_render, &parsed.async_renders, list)
    {
        ret = ts_render_join(curr_render->render);
        if(ret < 0)
        {
            err("Failed to join to async render\n");
            return ret;
        }

        list_del(&curr_render->list);
        free(curr_render);
    }

    list_for_each_entry_safe(curr_ts, n_ts, &parsed.trace_sets, list)
    {
        ts_close(curr_ts->set);
        list_del(&curr_ts->list);
        free(curr_ts);
    }
}
