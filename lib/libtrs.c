#include "libtrs.h"
#include "__libtrs_internal.h"

#include <stdlib.h>
#include <errno.h>

#include <stdint.h>
#include <stdbool.h>

/* --- static defines --- */

typedef enum header
{
    NUMBER_TRACES = 0x41, NUMBER_SAMPLES, SAMPLE_CODING, LENGTH_DATA, TITLE_SPACE, TRACE_TITLE,
    DESCRIPTION, OFFSET_X, LABEL_X, LABEL_Y, SCALE_X, SCALE_Y, TRACE_OFFSET, LOGARITHMIC_SCALE,
    ACQUISITION_RANGE_OF_SCOPE = 0x55, ACQUISITION_COUPLING_OF_SCOPE, ACQUISITION_OFFSET_OF_SCOPE,
    ACQUISITION_INPUT_IMPEDANCE, ACQUISITION_DEVICE_ID, ACQUISITION_TYPE_FILTER,
    ACQUISITION_FREQUENCY_FILTER, ACQUISITION_RANGE_FILTER, TRACE_BLOCK = 0x5F,
    EXTERNAL_CLOCK_USED, EXTERNAL_CLOCK_THRESHOLD, EXTERNAL_CLOCK_MULTIPLIER,
    EXTERNAL_CLOCK_PHASE_SHIFT, EXTERNAL_CLOCK_RESAMPLER_MASK, EXTERNAL_CLOCK_RESAMPLER_ENABLED,
    EXTERNAL_CLOCK_FREQUENCY, EXTERNAL_CLOCK_BASE, NUMBER_VIEW, TRACE_OVERLAP, GO_LAST_TRACE,
    INPUT_OFFSET, OUTPUT_OFFSET, KEY_OFFSET, INPUT_LENGTH, OUTPUT_LENGTH, KEY_LENGTH,
    NUMBER_OF_ENABLED_CHANNELS, NUMBER_OF_USED_OSCILLOSCOPES,
    XY_SCAN_WIDTH, XY_SCAN_HEIGHT, XY_MEASUREMENTS_PER_SPOT,
} header_t;

static const char header_desc[][80] = {
        [NUMBER_TRACES]                     = "Number of traces",
        [NUMBER_SAMPLES]                    = "Number of samples per trace",
        [SAMPLE_CODING]                     = "Sample Coding",
        [LENGTH_DATA]                       = "Length of cryptographic data included in trace",
        [TITLE_SPACE]                       = "Title space reserved per trace",
        [TRACE_TITLE]                       = "Global trace title",
        [DESCRIPTION]                       = "Description",
        [OFFSET_X]                          = "Offset in X-axis for trace representation",
        [LABEL_X]                           = "Label of X-axis",
        [LABEL_Y]                           = "Label of Y-axis",
        [SCALE_X]                           = "Scale value for X-axis",
        [SCALE_Y]                           = "Scale value for Y-axis",
        [TRACE_OFFSET]                      = "Trace offset for displaying trace numbers",
        [LOGARITHMIC_SCALE]                 = "Logarithmic scale",
        [ACQUISITION_RANGE_OF_SCOPE]        = "Range of the scope used to perform acquisition",
        [ACQUISITION_COUPLING_OF_SCOPE]     = "Coupling of the scope used to perform acquisition",
        [ACQUISITION_OFFSET_OF_SCOPE]       = "Offset of the scope used to perform acquisition",
        [ACQUISITION_INPUT_IMPEDANCE]       = "Input impedance of the scope used to perform acquisition",
        [ACQUISITION_DEVICE_ID]             = "Device ID of the scope used to perform acquisition",
        [ACQUISITION_TYPE_FILTER]           = "The type of filter used during acquisition",
        [ACQUISITION_FREQUENCY_FILTER]      = "Frequency of the filter used during acquisition",
        [ACQUISITION_RANGE_FILTER]          = "Range of the filter used during acquisition",
        [TRACE_BLOCK]                       = "Trace block marker: an empty TLV that marks the end of the header",
        [EXTERNAL_CLOCK_USED]               = "External clock used",
        [EXTERNAL_CLOCK_THRESHOLD]          = "External clock threshold",
        [EXTERNAL_CLOCK_MULTIPLIER]         = "External clock multiplier",
        [EXTERNAL_CLOCK_PHASE_SHIFT]        = "External clock phase shift",
        [EXTERNAL_CLOCK_RESAMPLER_MASK]     = "External clock resampler mask",
        [EXTERNAL_CLOCK_RESAMPLER_ENABLED]  = "External clock resampler enabled",
        [EXTERNAL_CLOCK_FREQUENCY]          = "External clock frequency",
        [EXTERNAL_CLOCK_BASE]               = "External clock time base",
        [NUMBER_VIEW]                       = "View number of traces: number of traces to show on opening",
        [TRACE_OVERLAP]                     = "Overlap: whether to overlap traces in case of multi trace view",
        [GO_LAST_TRACE]                     = "Go to last trace on opening",
        [INPUT_OFFSET]                      = "Input data offset in trace data",
        [OUTPUT_OFFSET]                     = "Output data offset in trace data",
        [KEY_OFFSET]                        = "Key data offset in trace data",
        [INPUT_LENGTH]                      = "Input data length in trace data",
        [OUTPUT_LENGTH]                     = "Output data length in trace data",
        [KEY_LENGTH]                        = "Key data length in trace data",
        [NUMBER_OF_ENABLED_CHANNELS]        = "Number of oscilloscope channels used for measurement",
        [NUMBER_OF_USED_OSCILLOSCOPES]      = "Number of oscilloscopes used for measurement",
        [XY_SCAN_WIDTH]                     = "Number of steps in the \"x\" direction during XY scan",
        [XY_SCAN_HEIGHT]                    = "Number of steps in the \"y\" direction during XY scan",
        [XY_MEASUREMENTS_PER_SPOT]          = "Number of consecutive measurements done per spot during XY scan",
};

#define __def_TH_INT   integer
#define __def_TH_STR   string
#define __def_TH_FLT   floating
#define __def_TH_BYTE  bytes
#define __def_TH_BOOL  boolean

#define HEADER_NAME(_index, _tag, _name, _req, \
                        _type, _len, _def)     \
    [(_index) == (_tag) ? (_index) : -1] =     \
        {                                      \
            .name = (_name), .req = (_req),    \
            .th_type = (_type), .len = (_len), \
            .desc = header_desc[(_index)],     \
            .def.__def_ ## _type = (_def)      \
        }

struct th_def
{
    char name[2];
    bool req;

    enum th_type
    {
        TH_INT, TH_FLT, TH_BOOL,
        TH_STR, TH_BYTE
    } th_type;

    size_t len;
    union
    {
        int integer;
        float floating;
        bool boolean;

        char *string;
        uint8_t *bytes;
    } def;
    const char *desc;
};

static const struct th_def all_headers[] = {
        HEADER_NAME(NUMBER_TRACES, 0x41, "NT", true, TH_INT, 4, 0),
        HEADER_NAME(NUMBER_SAMPLES, 0x42, "NS", true, TH_INT, 4, 0),
        HEADER_NAME(SAMPLE_CODING, 0x43, "SC", true, TH_INT, 1, 0),
        HEADER_NAME(LENGTH_DATA, 0x44, "DS", false, TH_INT, 2, 0),
        HEADER_NAME(TITLE_SPACE, 0x45, "TS", false, TH_INT, 1, 255),
        HEADER_NAME(TRACE_TITLE, 0x46, "GT", false, TH_STR, 0, "trace"),
        HEADER_NAME(DESCRIPTION, 0x47, "DC", false, TH_STR, 0, NULL),
        HEADER_NAME(OFFSET_X, 0x48, "XO", false, TH_INT, 4, 0),
        HEADER_NAME(LABEL_X, 0x49, "XL", false, TH_STR, 0, NULL),
        HEADER_NAME(LABEL_Y, 0x4A, "YL", false, TH_STR, 0, NULL),
        HEADER_NAME(SCALE_X, 0x4B, "XS", false, TH_FLT, 4, 1),
        HEADER_NAME(SCALE_Y, 0x4C, "YS", false, TH_FLT, 4, 1),
        HEADER_NAME(TRACE_OFFSET, 0x4D, "TO", false, TH_INT, 4, 0),
        HEADER_NAME(LOGARITHMIC_SCALE, 0x4E, "LS", false, TH_INT, 1, 0),
        HEADER_NAME(ACQUISITION_RANGE_OF_SCOPE, 0x55, "RG", false, TH_FLT, 4, 0),
        HEADER_NAME(ACQUISITION_COUPLING_OF_SCOPE, 0x56, "CL", false, TH_INT, 4, 0),
        HEADER_NAME(ACQUISITION_OFFSET_OF_SCOPE, 0x57, "OS", false, TH_FLT, 4, 0),
        HEADER_NAME(ACQUISITION_INPUT_IMPEDANCE, 0x58, "II", false, TH_FLT, 4, 0),
        HEADER_NAME(ACQUISITION_DEVICE_ID, 0x59, "AI", false, TH_BYTE, 0, NULL),
        HEADER_NAME(ACQUISITION_TYPE_FILTER, 0x5A, "FT", false, TH_INT, 4, 0),
        HEADER_NAME(ACQUISITION_FREQUENCY_FILTER, 0x5B, "FF", false, TH_FLT, 4, 0),
        HEADER_NAME(ACQUISITION_RANGE_FILTER, 0x5C, "FR", false, TH_FLT, 4, 0),
        HEADER_NAME(TRACE_BLOCK, 0x5F, "TB", true, TH_INT, 0, 0),
        HEADER_NAME(EXTERNAL_CLOCK_USED, 0x60, "EU", false, TH_BOOL, 1, false),
        HEADER_NAME(EXTERNAL_CLOCK_THRESHOLD, 0x61, "ET", false, TH_FLT, 4, 0),
        HEADER_NAME(EXTERNAL_CLOCK_MULTIPLIER, 0x62, "EM", false, TH_INT, 4, 0),
        HEADER_NAME(EXTERNAL_CLOCK_PHASE_SHIFT, 0x63, "EP", false, TH_INT, 4, 0),
        HEADER_NAME(EXTERNAL_CLOCK_RESAMPLER_MASK, 0x64, "ER", false, TH_INT, 4, 0),
        HEADER_NAME(EXTERNAL_CLOCK_RESAMPLER_ENABLED, 0x65, "RE", false, TH_BOOL, 1, false),
        HEADER_NAME(EXTERNAL_CLOCK_FREQUENCY, 0x66, "EF", false, TH_FLT, 4, 0),
        HEADER_NAME(EXTERNAL_CLOCK_BASE, 0x67, "EB", false, TH_INT, 4, 0),
        HEADER_NAME(NUMBER_VIEW, 0x68, "VT", false, TH_INT, 4, 0),
        HEADER_NAME(TRACE_OVERLAP, 0x69, "OV", false, TH_BOOL, 1, false),
        HEADER_NAME(GO_LAST_TRACE, 0x6A, "GL", false, TH_BOOL, 1, false),
        HEADER_NAME(INPUT_OFFSET, 0x6B, "IO", false, TH_INT, 4, 0),
        HEADER_NAME(OUTPUT_OFFSET, 0x6C, "OO", false, TH_INT, 4, 0),
        HEADER_NAME(KEY_OFFSET, 0x6D, "KO", false, TH_INT, 4, 0),
        HEADER_NAME(INPUT_LENGTH, 0x6E, "IL", false, TH_INT, 4, 0),
        HEADER_NAME(OUTPUT_LENGTH, 0x6F, "OL", false, TH_INT, 4, 0),
        HEADER_NAME(KEY_LENGTH, 0x70, "KL", false, TH_INT, 4, 0),
        HEADER_NAME(NUMBER_OF_ENABLED_CHANNELS, 0x71, "CH", false, TH_INT, 4, 0),
        HEADER_NAME(NUMBER_OF_USED_OSCILLOSCOPES, 0x72, "NO", false, TH_INT, 4, 0),
        HEADER_NAME(XY_SCAN_WIDTH, 0x73, "WI", false, TH_INT, 4, 0),
        HEADER_NAME(XY_SCAN_HEIGHT, 0x74, "HE", false, TH_INT, 4, 0),
        HEADER_NAME(XY_MEASUREMENTS_PER_SPOT, 0x75, "ME", false, TH_INT, 4, 0),
};

/* --- trace sets and trace headers --- */

struct th_data
{
    uint8_t tag;
    union
    {
        int integer;
        float floating;
        bool boolean;

        char *string;
        uint8_t *bytes;
    } val;
};

#if SUPPORT_PTHREAD
#define ts_lock(set, out)   \
    if(sem_wait(&(set)->file_lock) < 0) out

#define ts_unlock(set, out) \
    if(sem_post(&(set)->file_lock) < 0) out
#else
#define ts_lock(set, out)   ;
#define ts_unlock(set, out) ;
#endif

int __read_tag_and_len(FILE *ts_file, uint8_t *tag, uint32_t *actual_len)
{
    uint8_t len;
    size_t ret;

    ret = fread(tag, 1, 1, ts_file);
    if(ret != 1)
        return -EIO;

    ret = fread(&len, 1, 1, ts_file);
    if(ret != 1)
        return -EIO;

    if(len & 0x80)
    {
        *actual_len = 0;
        ret = fread(actual_len, 1, len & 0x7F, ts_file);
        if(ret != (len & 0x7F))
            return -EIO;
    }
    else *actual_len = len;

    return 0;
}

void dump_headers(struct trace_set *ts)
{
    int i;
    for(i = 0; i < ts->num_headers; i++)
    {
        printf("%s: ", all_headers[ts->headers[i].tag].desc);
        switch(all_headers[ts->headers[i].tag].th_type)
        {
            case TH_INT:
                printf("%i\n", ts->headers[i].val.integer);
                break;

            case TH_FLT:
                printf("%f\n", ts->headers[i].val.floating);
                break;

            case TH_BOOL:
                printf("%i\n", ts->headers[i].val.boolean);
                break;

            case TH_BYTE:
            case TH_STR:
                printf("%s\n", ts->headers[i].val.string);
                break;
        }
    }
}

int __parse_headers(struct trace_set *ts)
{
    int i, j, stat;
    size_t ret;

    uint8_t tag;
    uint32_t actual_len;

    ts->num_traces = -1;
    ts->num_samples = -1;
    ts->title_size = -1;
    ts->data_size = -1;
    ts->datatype = DT_NONE;

    ts->input_offs = -1;
    ts->input_len = -1;
    ts->output_offs = -1;
    ts->output_len = -1;
    ts->key_offs = -1;
    ts->key_len = -1;

    ts->yscale = 0.0;

    for(i = 0; i < ts->num_headers; i++)
    {
        stat = __read_tag_and_len(ts->ts_file, &tag, &actual_len);
        if(stat < 0)
            return stat;

        ts->headers[i].tag = tag;
        switch(all_headers[tag].th_type)
        {
            case TH_INT:
            case TH_FLT:
            case TH_BOOL:
                ret = fread(&ts->headers[i].val, 1, actual_len, ts->ts_file);
                if(ret != actual_len)
                {
                    stat = -EIO;
                    goto __fail;
                }
                break;

            case TH_STR:
            case TH_BYTE:
                ts->headers[i].val.bytes = calloc(actual_len, 1);
                if(!ts->headers[i].val.bytes)
                {
                    stat = -EIO;
                    goto __fail;
                }

                ret = fread(ts->headers[i].val.bytes, 1, actual_len, ts->ts_file);
                if(ret != actual_len)
                {
                    stat = -EIO;
                    goto __fail;
                }
                break;

            default:
                return -EINVAL;
        }

        if(tag == NUMBER_TRACES)
            ts->num_traces = ts->headers[i].val.integer;
        else if(tag == NUMBER_SAMPLES)
            ts->num_samples = ts->headers[i].val.integer;

        else if(tag == TITLE_SPACE)
            ts->title_size = ts->headers[i].val.integer;
        else if(tag == LENGTH_DATA)
            ts->data_size = ts->headers[i].val.integer;
        else if(tag == SAMPLE_CODING)
            ts->datatype = ts->headers[i].val.integer;

        else if(tag == INPUT_OFFSET)
            ts->input_offs = ts->headers[i].val.integer;
        else if(tag == INPUT_LENGTH)
            ts->input_len = ts->headers[i].val.integer;
        else if(tag == OUTPUT_OFFSET)
            ts->output_offs = ts->headers[i].val.integer;
        else if(tag == OUTPUT_LENGTH)
            ts->output_len = ts->headers[i].val.integer;
        else if(tag == KEY_OFFSET)
            ts->key_offs = ts->headers[i].val.integer;
        else if(tag == KEY_LENGTH)
            ts->key_len = ts->headers[i].val.integer;
        else if(tag == SCALE_Y)
            ts->yscale = ts->headers[i].val.floating;
    }

    // others are okay to not have
    if(ts->num_traces == -1 || ts->num_samples == -1 ||
       ts->title_size == -1 || ts->data_size == -1 ||
       ts->datatype == DT_NONE)
    {
        return -EINVAL;
    }

    ts->trace_length = ts->num_samples * (ts->datatype & 0xF) + ts->data_size + ts->title_size;
    ts->trace_start = ftell(ts->ts_file);
    return 0;

__fail:
    for(j = 0; j <= i; j++)
    {
        // valid entry
        if(ts->headers[i].tag != 0)
        {
            if(all_headers[ts->headers[i].tag].th_type == TH_STR ||
               all_headers[ts->headers[i].tag].th_type == TH_BYTE)
            {
                if(ts->headers[i].val.bytes != NULL)
                    free(ts->headers[i].val.bytes);
            }
        }
    }

    return stat;
}

int __num_headers(FILE *ts_file)
{
    int count = 0, stat;
    size_t ret, start;
    uint8_t tag = 0, data[255];
    uint32_t actual_len;

    if(!ts_file)
        return -EINVAL;

    start = ftell(ts_file);
    while(tag != TRACE_BLOCK)
    {
        stat = __read_tag_and_len(ts_file, &tag, &actual_len);
        count++;

        if(stat < 0)
            return stat;

        if(actual_len > sizeof(data))
            return -ENOMEM;

        if(actual_len == 0)
            continue;

        ret = fread(data, 1, actual_len, ts_file);
        if(ret != actual_len)
            return -EIO;

    }

    fseek(ts_file, start, SEEK_SET);
    return count;
}

int ts_open(struct trace_set **ts, const char *path)
{
    int retval;
    struct trace_set *ts_result;

    if(!ts || !path)
        return -EINVAL;

    ts_result = calloc(1, sizeof(struct trace_set));
    if(!ts_result)
        return -ENOMEM;

    ts_result->ts_file = fopen(path, "rb");
    if(!ts_result->ts_file)
    {
        retval = -ENOENT;
        goto __free_ts_result;
    }

    retval = __num_headers(ts_result->ts_file);
    if(retval < 0)
        goto __close_ts_file;

    ts_result->num_headers = retval;
    ts_result->headers = (struct th_data *) calloc(retval, sizeof(struct th_data));
    if(!ts_result->headers)
    {
        retval = -ENOMEM;
        goto __close_ts_file;
    }

    retval = __parse_headers(ts_result);
    if(retval < 0)
        goto __free_headers;

#if SUPPORT_PTHREAD
    retval = sem_init(&ts_result->file_lock, 0, 1);
    if(retval < 0)
    {
        retval = errno;
        goto __free_headers;
    }
#endif

    ts_result->prev = NULL;
    ts_result->tfm = NULL;

    *ts = ts_result;
    return 0;

__free_headers:
    free(ts_result->headers);

__close_ts_file:
    fclose(ts_result->ts_file);

__free_ts_result:
    free(ts_result);

    *ts = NULL;
    return retval;
}

int ts_close(struct trace_set *ts)
{
    int i;
    if(!ts)
        return -EINVAL;

    // wait for any consumers
    ts_lock(ts, ;)

#if SUPPORT_PTHREAD
    sem_destroy(&ts->file_lock);
#endif

    for(i = 0; i < ts->num_headers; i++)
    {
        if(all_headers[ts->headers[i].tag].th_type == TH_STR ||
           all_headers[ts->headers[i].tag].th_type == TH_BYTE)
            free(ts->headers[i].val.bytes);
    }

    if(ts->headers)
        free(ts->headers);

    if(ts->ts_file)
        fclose(ts->ts_file);

    if(ts->prev && ts->tfm)
        ts->tfm->exit(ts);

    free(ts);
    return 0;
}

int ts_create(struct trace_set **ts, struct trace_set *from, const char *path)
{
    fprintf(stderr, "ts_create currently unsupported\n");
    return -1;
}

int ts_append(struct trace_set *ts, struct trace *t)
{
    fprintf(stderr, "ts_append currently unsupported\n");
    return -1;
}

int ts_transform(struct trace_set **new_ts, struct trace_set *prev, struct tfm *transform)
{
    int ret;
    struct trace_set *ts_result;

    if(!new_ts || !prev || !transform)
        return -EINVAL;

    ts_result = calloc(1, sizeof(struct trace_set));
    if(!ts_result)
        return -ENOMEM;

    // no need to seek within a file or parse headers
    ts_result->ts_file = NULL;
    ts_result->trace_length = 1;
    ts_result->trace_start = 0;

    ts_result->num_headers = 0;
    ts_result->headers = NULL;

    // link previous set
    ts_result->prev = prev;
    ts_result->tfm = transform;

    // transform-specific initialization
    ret = transform->init(ts_result);
    if(ret < 0)
    {
        free(ts_result);
        return ret;
    }

    *new_ts = ts_result;
    return 0;
}

size_t ts_num_traces(struct trace_set *ts)
{
    if(!ts)
        return -EINVAL;

    return ts->num_traces;
}

size_t ts_num_samples(struct trace_set *ts)
{
    if(!ts)
        return -EINVAL;

    return ts->num_samples;
}

/* --- trace operations --- */

size_t ts_trace_size(struct trace_set *ts)
{
    if(!ts)
        return -EINVAL;

    return ts->num_samples * sizeof(float) +
           ts->data_size + ts->title_size +
           sizeof(struct trace);
}

int trace_get(struct trace_set *ts, struct trace **t, size_t index, bool prebuffer)
{
    int retval;
    struct trace *t_result;
    if(!ts || !t)
        return -EINVAL;

    if(index >= ts->num_traces)
        return -EFAULT;

    t_result = calloc(1, sizeof(struct trace));
    if(!t_result)
        return -ENOMEM;

    t_result->owner = ts;
    t_result->start_offset = ts->trace_start + index * ts->trace_length;
    t_result->buffered_title = NULL;
    t_result->buffered_data = NULL;
    t_result->buffered_samples = NULL;

    if(prebuffer)
    {
        retval = trace_title(t_result, &t_result->buffered_title);
        if(retval < 0)
            goto __fail;

        retval = trace_data_all(t_result, &t_result->buffered_data);
        if(retval < 0)
            goto __fail;

        retval = trace_samples(t_result, &t_result->buffered_samples);
        if(retval < 0)
            goto __fail;
    }

    *t = t_result;
    return 0;

__fail:
    trace_free(t_result);
    *t = NULL;
    return retval;
}

int trace_free(struct trace *t)
{
    if(!t)
        return -EINVAL;

    if(t->owner->prev && t->owner->tfm)
    {
        if(t->buffered_title)
            t->owner->tfm->free_title(t);

        if(t->buffered_data)
            t->owner->tfm->free_data(t);

        if(t->buffered_samples)
            t->owner->tfm->free_samples(t);
    }
    else
    {
        if(t->buffered_title)
            free(t->buffered_title);

        if(t->buffered_data)
            free(t->buffered_data);

        if(t->buffered_samples)
            free(t->buffered_samples);
    }

    free(t);
    return 0;
}

int trace_title(struct trace *t, char **title)
{
    int stat;
    size_t ret;
    char *result;

    if(!t || !title)
        return -EINVAL;

    if(t->buffered_title)
    {
        *title = t->buffered_title;
        return 0;
    }

    if(t->owner->prev && t->owner->tfm)
    {
        stat = t->owner->tfm->title(t, &result);
        if(stat < 0)
            goto __fail;
    }
    else
    {
        result = calloc(1, t->owner->title_size);
        if(!result)
            return -ENOMEM;

        ts_lock(t->owner, goto __sem_fail);

        stat = fseek(t->owner->ts_file, t->start_offset, SEEK_SET);
        if(stat)
        {
            stat = -EIO;
            goto __free_result;
        }

        ret = fread(result, 1, t->owner->title_size, t->owner->ts_file);
        if(ret != t->owner->title_size)
        {
            stat = -EIO;
            goto __free_result;
        }

        ts_unlock(t->owner, goto __sem_fail);
    }

    t->buffered_title = result;
    *title = result;
    return 0;

#if SUPPORT_PTHREAD
__sem_fail:
    stat = errno;
#endif

__free_result:
    free(result);

__fail:
    return stat;
}

int __trace_buffer_data(struct trace *t)
{
    int stat;
    size_t ret;
    uint8_t *result;

    if(t->owner->prev && t->owner->tfm)
    {
        stat = t->owner->tfm->data(t, &result);
        if(stat < 0)
            goto __fail;
    }
    else
    {
        result = calloc(1, t->owner->data_size);
        if(!result)
            return -ENOMEM;

        ts_lock(t->owner, goto __sem_fail);
        stat = fseek(t->owner->ts_file, t->start_offset + t->owner->title_size, SEEK_SET);
        if(stat)
        {
            stat = -EIO;
            goto __free_result;
        }

        ret = fread(result, 1, t->owner->data_size, t->owner->ts_file);
        if(ret != t->owner->data_size)
        {
            stat = -EIO;
            goto __free_result;
        }
        ts_unlock(t->owner, goto __sem_fail);
    }

    t->buffered_data = result;
    return 0;

#if SUPPORT_PTHREAD
__sem_fail:
    stat = errno;
#endif

__free_result:
    free(result);

__fail:
    return stat;
}

int __trace_data_generic(struct trace *t, uint8_t **data,
                         size_t offs, size_t len)
{
    int stat;
    if(t->buffered_data)
    {
        *data = &t->buffered_data[offs];
        return len;
    }

    if(offs == -1 || len == -1)
    {
        *data = NULL;
        return 0;
    }

    stat = __trace_buffer_data(t);
    if(stat < 0)
    {
        *data = NULL;
        return stat;
    }

    *data = &t->buffered_data[offs];
    return len;
}

int trace_data_all(struct trace *t, uint8_t **data)
{
    if(!t || !data)
        return -EINVAL;

    return __trace_data_generic(t, data, 0, t->owner->data_size);
}

int trace_data_input(struct trace *t, uint8_t **data)
{
    if(!t || !data)
        return -EINVAL;

    return __trace_data_generic(t, data,
                                t->owner->input_offs,
                                t->owner->input_len);
}

int trace_data_output(struct trace *t, uint8_t **data)
{
    if(!t || !data)
        return -EINVAL;

    return __trace_data_generic(t, data,
                                t->owner->output_offs,
                                t->owner->output_len);
}

int trace_data_key(struct trace *t, uint8_t **data)
{
    if(!t || !data)
        return -EINVAL;

    return __trace_data_generic(t, data,
                                t->owner->key_offs,
                                t->owner->key_len);
}

size_t trace_samples(struct trace *t, float **data)
{
    int stat, i;
    size_t ret;
    void *temp;
    float *result;

    if(!t || !data)
        return -EINVAL;

    if(t->buffered_samples)
    {
        *data = t->buffered_samples;
        return 0;
    }

    if(t->owner->prev && t->owner->tfm)
    {
        stat = t->owner->tfm->samples(t, &result);
        if(stat < 0)
            goto __fail;
    }
    else
    {
        temp = calloc(t->owner->datatype & 0xF, t->owner->num_samples);
        if(!temp)
            return -ENOMEM;

        result = calloc(sizeof(float), t->owner->num_samples);
        if(!result)
        {
            stat = -ENOMEM;
            goto __free_temp;
        }

        ts_lock(t->owner, goto __sem_fail);
        stat = fseek(t->owner->ts_file,
                     t->start_offset + t->owner->title_size + t->owner->data_size,
                     SEEK_SET);
        if(stat)
        {
            stat = -EIO;
            goto __free_temp;
        }

        ret = fread(temp, t->owner->datatype & 0xF, t->owner->num_samples, t->owner->ts_file);
        if(ret != t->owner->num_samples)
        {
            stat = -EIO;
            goto __free_temp;
        }
        ts_unlock(t->owner, goto __sem_fail);

        switch(t->owner->datatype)
        {
            case DT_BYTE:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * (float) ((char *) temp)[i];
                break;

            case DT_SHORT:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * (float) ((short *) temp)[i];
                break;

            case DT_INT:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * (float) ((int *) temp)[i];
                break;

            case DT_FLOAT:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * ((float *) temp)[i];
                break;

            case DT_NONE:
                goto __free_result;
        }

        free(temp);
    }

    t->buffered_samples = result;
    *data = result;
    return 0;

#if SUPPORT_PTHREAD
__sem_fail:
    stat = errno;
#endif

__free_result:
    free(result);

__free_temp:
    free(temp);

__fail:
    return stat;
}
