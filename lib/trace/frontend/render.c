#include "trace.h"
#include "__trace_internal.h"

#include "platform.h"
#include <stdlib.h>

struct __ts_render_arg
{
    int thread_index;
    LT_SEM_TYPE *done_signal;
    LT_SEM_TYPE thread_signal;

    struct trace_set *ts;
    size_t trace_index;
    int ret;
};

#include "statistics.h"

LT_THREAD_FUNC(__ts_render_func, thread_arg)
{
    int ret;
    struct __ts_render_arg *arg = (struct __ts_render_arg *) thread_arg;
    struct trace *trace;

    struct accumulator *acc;
    float maxabs;

    stat_create_single(&acc, STAT_MAXABS);

    debug("Hello from thread %i, ts %p\n", arg->thread_index, arg->ts);
    while(1)
    {
        sem_acquire(&arg->thread_signal);
        debug("Thread %i has woken up with trace set %p\n", arg->thread_index, arg->ts);

        // exit condition
        if(arg->ts == NULL)
        {
            debug("Thread %i exiting cleanly\n", arg->thread_index);
            arg->ret = 0;
            return NULL;
        }

        debug("Working on trace %zu\n", arg->trace_index);

        ret = trace_get(arg->ts, &trace, arg->trace_index);
        if(ret < 0)
        {
            err("Thread %i failed to get trace at index %zu\n",
                arg->thread_index, arg->trace_index);
            arg->ret = ret;

            sem_release(arg->done_signal);
            return NULL;
        }

        stat_reset_accumulator(acc);
        stat_accumulate_single_many(acc, trace->samples, trace->owner->num_samples);
        stat_get(acc, STAT_MAXABS, 0, &maxabs);

        if(arg->trace_index % 65536 == 0)
            printf("\n");
        else
            printf(",");
        printf("%0.5f", maxabs);

        if(arg->trace_index % 65536 < 8)
            err("%s\n", trace->title);
        if(arg->trace_index % 65536 == 8)
            err("\n");

        trace_free(trace);

        arg->ret = 1;
        sem_release(arg->done_signal);
    }

    err("Thread %i has finished\n", arg->thread_index);
}

struct render
{
    LT_THREAD_TYPE handle;
    int ret;

    struct trace_set *ts;
    size_t nthreads;
};

LT_THREAD_FUNC(__ts_render_controller, controller_arg)
{
    int i, j, ret;
    size_t curr_index = 0;
    LT_SEM_TYPE done_signal;

    LT_THREAD_TYPE *handles;
    struct __ts_render_arg *args;
    struct render *arg = controller_arg;

    ret = p_sem_create(&done_signal, arg->nthreads);
    if(ret < 0)
    {
        err("Failed to initialize done semaphore\n");
        arg->ret = -errno;
        return NULL;
    }

    handles = calloc(arg->nthreads, sizeof(LT_THREAD_TYPE));
    if(!handles)
    {
        err("Unable to allocate pthread handles\n");
        ret = -ENOMEM;
        goto __destroy_done_signal;
    }

    args = calloc(arg->nthreads, sizeof(struct __ts_render_arg));
    if(!args)
    {
        err("Unable to allocate pthread args\n");
        ret = -ENOMEM;
        goto __free_handles;
    }

    for(i = 0; i < arg->nthreads; i++)
    {
        args[i].thread_index = i;
        args[i].done_signal = &done_signal;
        args[i].ts = arg->ts;
        args[i].trace_index = 0;
        args[i].ret = 1;

        ret = p_sem_create(&args[i].thread_signal, 0);
        if(ret < 0)
        {
            err("Unable to allocate thread signal for thread %i\n", i);
            ret = -errno;
            goto __teardown_args;
        }
    }

    for(i = 0; i < arg->nthreads; i++)
    {
        ret = p_thread_create(&handles[i], __ts_render_func, &args[i]);
        if(ret < 0)
        {
            err("Unable to create pthread %i\n", i);
            ret = -errno;
            goto __kill_threads;
        }
    }

    while(curr_index < ts_num_traces(arg->ts))
    {
        debug("Waiting for worker thread\n");
        sem_acquire(&done_signal);

        for(i = 0; i < arg->nthreads; i++)
        {
            if(args[i].ret == 1)
            {
                debug("Dispatching index %zu to thread %i\n", curr_index, i);

                args[i].trace_index = curr_index++;
                args[i].ret = 0;

                sem_release(&args[i].thread_signal);
                break;
            }
            else if(args[i].ret < 0)
            {
                err("Detected error for thread %i\n", i);

                ret = args[i].ret;
                goto __done;
            }
        }
    }

    debug("Done, waiting for workers to finish\n");
    for(i = 0; i < arg->nthreads; i++)
        sem_acquire(&done_signal);

    ret = 0;
__done:
    i = arg->nthreads;
__kill_threads:
    for(j = 0; j < i; j++)
    {
        args[j].ts = NULL;

        sem_release(&args[j].thread_signal);
        p_thread_join(handles[j]);
    }

    i = arg->nthreads;
__teardown_args:
    for(j = 0; j < i; j++)
        p_sem_destroy(&args[j].thread_signal);
    free(args);

__free_handles:
    free(handles);

__destroy_done_signal:
    p_sem_destroy(&done_signal);

    arg->ret = ret;
    return NULL;
}

int ts_render(struct trace_set *ts, size_t nthreads)
{
    struct render arg = {
            .ret = 0,
            .ts = ts,
            .nthreads = nthreads
    };

    if(!ts || nthreads == 0)
    {
        err("Invalid trace set or number of threads\n");
        return -EINVAL;
    }

    __ts_render_controller(&arg);
    return arg.ret;
}

int ts_render_async(struct trace_set *ts, size_t nthreads, struct render **render)
{
    int ret;
    struct render *res;

    if(!ts || nthreads == 0)
    {
        err("Invalid trace set or number of threads\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct render));
    if(!res)
    {
        err("Failed to allocate render struct\n");
        return -ENOMEM;
    }

    res->ret = 0;
    res->ts = ts;
    res->nthreads = nthreads;

    ret = p_thread_create(&res->handle, __ts_render_controller, res);
    if(ret < 0)
    {
        err("Failed to create controller pthread\n");
        free(res);
        return -EINVAL;
    }

    *render = res;
    return 0;
}

int ts_render_join(struct render *render)
{
    int ret;
    if(!render)
    {
        err("Invalid render struct\n");
        return -EINVAL;
    }

    p_thread_join(render->handle);
    ret = render->ret;

    free(render);
    return ret;
}
