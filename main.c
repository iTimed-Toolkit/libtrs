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
        STR_AT_IDX(PORT_CPA_SPLIT_PM_PROGRESS),
        STR_AT_IDX(PORT_EXTRACT_PATTERN_DEBUG)
};

static const char *fill_order_t_strings[] = {
        STR_AT_IDX(ROWS),
        STR_AT_IDX(COLS),
        STR_AT_IDX(PLOTS)
};

static const char *crypto_t_strings[] = {
        STR_AT_IDX(AES128)
};

static const char *summary_t_strings[] = {
        STR_AT_IDX(SUMMARY_AVG),
        STR_AT_IDX(SUMMARY_DEV),
        STR_AT_IDX(SUMMARY_MIN),
        STR_AT_IDX(SUMMARY_MAX)
};

static const char *filter_t_strings[] = {
        STR_AT_IDX(ALONG_NUM),
        STR_AT_IDX(ALONG_DATA)
};

static const char *aes_leakage_t_strings[] = {
        STR_AT_IDX(AES128_R0_R1_HD_NOMC),
        STR_AT_IDX(AES128_RO_HW_ADDKEY_OUT),
        STR_AT_IDX(AES128_R0_HW_SBOX_OUT),
        STR_AT_IDX(AES128_R10_OUT_HD),
        STR_AT_IDX(AES128_R10_HW_SBOXIN)
};


#define NUM_TABLE_ENTRIES(table)        (sizeof(table) / (sizeof((table)[0])))
#define IF_NEXT(c, dothis)              if(*(c)) { dothis; }
#define IF_NOT_NEXT(c, dothis)          if(!(*(c))) { dothis; }
#define CHECK_NEXT(c, fail)             IF_NOT_NEXT(c, err("CHECK_NEXT FAILED\n"); fail)

#define PARSE_ENUM_FUNC(type, table)                \
    int __parse_enum_ ## type(char **config,        \
                                type *res) {        \
    char *tok = strsep(config, SEPARATORS);         \
    for(int i = 0;                                  \
        i < NUM_TABLE_ENTRIES(table); i++) {        \
        if(strcmp(tok, (table)[i]) == 0) {          \
            *res = i; return 0; } }                 \
    err("No matching enum found in table for %s\n", tok);       \
    return -EINVAL; }

PARSE_ENUM_FUNC(port_t, port_t_strings);
PARSE_ENUM_FUNC(fill_order_t, fill_order_t_strings);
PARSE_ENUM_FUNC(crypto_t, crypto_t_strings);
PARSE_ENUM_FUNC(summary_t, summary_t_strings);
PARSE_ENUM_FUNC(filter_t, filter_t_strings);
PARSE_ENUM_FUNC(aes_leakage_t, aes_leakage_t_strings);

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

#define __parse_enum_nodecl(name, type, c)         \
    CHECK_NEXT(c, return -EINVAL);                  \
    ret = __parse_enum_ ## type (c, &(name));           \
    if(ret < 0) {                                   \
        err("Failed to parse " #name " enum\n");      \
        return ret; }

#define parse_arg(name, type, c)                    \
    type ## _tt name = init_ ## type;               \
    __parse_arg_nodecl(name, type, c)

#define parse_enum(name, type, c)                  \
    type name = init_enum;                         \
    __parse_enum_nodecl(name, type, c)

#define parse_struct(name, type, c, ...)            \
    type name; __VA_ARGS__;

#define parse_match_region_t(name, c)               \
    match_region_t name;                              \
    __parse_arg_nodecl((name).ref_trace, size_t, c);  \
    __parse_arg_nodecl((name).lower, int, c);         \
    __parse_arg_nodecl((name).upper, int, c);         \
    __parse_arg_nodecl((name).confidence, double, c);

#define parse_arg_optional(name, type, c)           \
    type ## _tt name = init_ ## type;               \
    IF_NEXT(c, __parse_arg_nodecl(name, type, c))

#define PARSE_FUNC(tfm_name, exec, ...)             \
    int __parse_ ## tfm_name (char **config,        \
                            struct tfm **tfm) {     \
    int ret; exec;                                  \
    ret = tfm_name(tfm, ## __VA_ARGS__);            \
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

PARSE_FUNC(tfm_synchronize,
           parse_arg(max_distance, int, config),
           max_distance);

PARSE_FUNC(tfm_wait_on,
           parse_enum(port, port_t, config);
                   parse_arg(bufsize, memsize, config),
           port, bufsize)

PARSE_FUNC(tfm_visualize,
           struct viz_args args = {.filename = NULL};
                   __parse_arg_nodecl(args.rows, int, config);
                   __parse_arg_nodecl(args.cols, int, config);
                   __parse_arg_nodecl(args.plots, int, config);
                   __parse_arg_nodecl(args.samples, int, config);

                   for(int i = 0; i < 3; i++) {
                   __parse_enum_nodecl(args.order[i], fill_order_t, config);
           }

                   // optionally get filename
                   IF_NEXT(config, if(*(*config) != '(' && *(*config) != '\0') { __parse_arg_nodecl(args.filename, string, config)) },
           &args)

PARSE_FUNC(tfm_average,
           parse_arg(per_sample, bool, config),
           per_sample)

PARSE_FUNC(tfm_verify,
           parse_enum(which, crypto_t, config),
           which);

PARSE_FUNC(tfm_reduce_along,
           parse_enum(stat, summary_t, config);
           parse_enum(along, filter_t, config);
           filter_param_t param;
           if(along == ALONG_NUM)
               __parse_arg_nodecl(param.num, int, config),
           stat, along, param);

PARSE_FUNC(tfm_select_along,
           parse_enum(stat, summary_t, config);
           parse_enum(along, filter_t, config);
           filter_param_t param;
           if(along == ALONG_NUM)
               __parse_arg_nodecl(param.num, int, config),
           stat, along, param);

PARSE_FUNC(tfm_extract_pattern,
           parse_arg(pattern_size, int, config);
                   parse_arg(expecting, int, config);
                   parse_arg(avg_len, int, config);
                   parse_arg(max_dev, int, config);
                   parse_match_region_t(pattern, config);
                   parse_enum(data, crypto_t, config),
                   pattern_size, expecting, avg_len, max_dev,
                   &pattern, data);

PARSE_FUNC(tfm_split_tvla,
           parse_arg(which, bool, config),
           which)

PARSE_FUNC(tfm_narrow,
           parse_arg(first_trace, int, config);
                   parse_arg(num_traces, int, config);
                   parse_arg(first_sample, int, config);
                   parse_arg(num_samples, int, config),
           first_trace, num_traces, first_sample, num_samples)

PARSE_FUNC(tfm_append,
           parse_arg(path, string, config),
           path);

PARSE_FUNC(tfm_static_align,
           parse_match_region_t(match, config);
           parse_arg(max_shift, int, config),
           &match, max_shift);

PARSE_FUNC(tfm_match,
           parse_match_region_t(first, config);
           parse_match_region_t(last, config);
           parse_match_region_t(pattern, config);
           parse_arg(avg_len, int, config);
           parse_arg(max_dev, int, config),
           &first, &last, &pattern, avg_len, max_dev);

PARSE_FUNC(tfm_io_correlation,
           parse_arg(verify_data, bool, config)
           parse_arg(granularity, int, config);
                   parse_arg(num, int, config),
           verify_data, granularity, num)

PARSE_FUNC(tfm_aes_intermediate,
           parse_enum(model, aes_leakage_t, config),
           model)

PARSE_FUNC(tfm_aes_knownkey,)

struct async_entry
{
    struct list_head list;
    struct render *render;
    struct export *export;
};

struct trace_set_entry
{
    struct list_head list;
    struct trace_set *set;
};

struct parse_args
{
    struct list_head async;
    struct list_head trace_sets;

    struct trace_set *main;
    size_t main_nthreads;
    int main_port;
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

    parsed->main = ts;
    parsed->main_nthreads = nthreads;
    return 0;
}

int parse_render_async(char **config, struct trace_set *ts, struct parse_args *parsed)
{
    int ret;
    struct async_entry *entry;

    parse_arg(nthreads, size_t, config);

    entry = calloc(1, sizeof(struct async_entry));
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

    list_add(&entry->list, &parsed->async);
    return 0;
}

int parse_export(char **config, struct trace_set *ts, struct parse_args *parsed)
{
    int ret;
    parse_arg(port, int, config);

    parsed->main = ts;
    parsed->main_port = port;
    return 0;
}

int parse_export_async(char **config, struct trace_set *ts, struct parse_args *parsed)
{
    int ret;
    struct async_entry *entry;

    parse_arg(port, int, config);

    entry = calloc(1, sizeof(struct async_entry));
    if(!entry)
    {
        err("Failed to allocate async export entry\n");
        return -ENOMEM;
    }

    ret = ts_export_async(ts, port, &entry->export);
    if(ret < 0)
    {
        err("Failed to create async export\n");
        return ret;
    }

    list_add(&entry->list, &parsed->async);
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
            if(parsed->main)
            {
                err("Duplicate main frontends not supported\n");
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
        else if(strcmp(type, "export") == 0)
        {
            if(parsed->main)
            {
                err("Duplicate main frontends not supported\n");
                return -EINVAL;
            }
            else
            {
                ret = parse_export(config, ts, parsed);
                if(ret < 0)
                {
                    err("Failed to parse main export\n");
                    return ret;
                }
            }
        }
        else if(strcmp(type, "export_async") == 0)
        {
            ret = parse_export_async(config, ts, parsed);
            if(ret < 0)
            {
                err("Failed to parse async export\n");
                return ret;
            }
        }
        else
        {
            err("Invalid extra argument: %s\n", type);
            return -EINVAL;
        }
    }

    return 0;
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
    else if(strcmp(type, "synchronize") == 0)
        ret = __parse_tfm_synchronize(&curr, &tfm);
    else if(strcmp(type, "wait_on") == 0)
        ret = __parse_tfm_wait_on(&curr, &tfm);
    else if(strcmp(type, "visualize") == 0)
        ret = __parse_tfm_visualize(&curr, &tfm);

        // analysis
    else if(strcmp(type, "average") == 0)
        ret = __parse_tfm_average(&curr, &tfm);
    else if(strcmp(type, "verify") == 0)
        ret = __parse_tfm_verify(&curr, &tfm);
    else if(strcmp(type, "reduce_along") == 0)
        ret = __parse_tfm_reduce_along(&curr, &tfm);
    else if(strcmp(type, "select_along") == 0)
        ret = __parse_tfm_select_along(&curr, &tfm);
    else if(strcmp(type, "extract_pattern") == 0)
        ret = __parse_tfm_extract_pattern(&curr, &tfm);

        // traces
    else if(strcmp(type, "split_tvla") == 0)
        ret = __parse_tfm_split_tvla(&curr, &tfm);
    else if(strcmp(type, "narrow") == 0)
        ret = __parse_tfm_narrow(&curr, &tfm);
    else if(strcmp(type, "append") == 0)
        ret = __parse_tfm_append(&curr, &tfm);

        // alignment
    else if(strcmp(type, "static_align") == 0)
        ret = __parse_tfm_static_align(&curr, &tfm);
    else if(strcmp(type, "match") == 0)
        ret = __parse_tfm_match(&curr, &tfm);

        // correlation
    else if(strcmp(type, "io_correlation") == 0)
        ret = __parse_tfm_io_correlation(&curr, &tfm);
    else if(strcmp(type, "aes_intermediate") == 0)
        ret = __parse_tfm_aes_intermediate(&curr, &tfm);
    else if(strcmp(type, "aes_knownkey") == 0)
        ret = __parse_tfm_aes_knownkey(&curr, &tfm);

        // comments
    else if(strcmp(type, ";") == 0 ||
            strcmp(type, "#") == 0)
        return 1;

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

                if(ret != 1)
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

    struct async_entry *curr_render, *n_render;
    struct trace_set_entry *curr_ts, *n_ts;

    struct parse_args parsed;
    LIST_HEAD_INIT_INLINE(parsed.async);
    LIST_HEAD_INIT_INLINE(parsed.trace_sets);

    if(argc != 2)
    {
        err("Usage: libtrace_evaluate [cfg filename]\n");
        return -EINVAL;
    }

    parsed.main = NULL;
    parsed.main_nthreads = -1;
    parsed.main_port = -1;

    ret = parse_config(argv[1], &parsed);
    if(ret < 0)
    {
        err("Failed to parse config\n");
        return ret;
    }

    if(parsed.main_nthreads != -1)
    {
        if(parsed.main_port == -1)
        {
            ret = ts_render(parsed.main, parsed.main_nthreads);
            if(ret < 0)
            {
                err("Failed to render main trace set\n");
                return ret;
            }
        }
        else
        {
            err("Found both main renders and exports\n");
            return -EINVAL;
        }

    }
    else if(parsed.main_port != -1)
    {
        ret = ts_export(parsed.main, parsed.main_port);
        if(ret < 0)
        {
            err("Failed to export main trace set\n");
            return ret;
        }
    }
    else
    {
        err("Found neither main render or export\n");
        return -EINVAL;
    }

    list_for_each_entry_safe(curr_render, n_render, &parsed.async, list)
    {
        if(curr_render->render)
        {
            ret = ts_render_join(curr_render->render);
            if(ret < 0)
            {
                err("Failed to join to async render\n");
                return ret;
            }
        }
        else if(curr_render->export)
        {
            ret = ts_export_join(curr_render->export);
            if(ret < 0)
            {
                err("Failed to join to async export\n");
                return ret;
            }
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
