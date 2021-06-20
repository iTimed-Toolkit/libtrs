#include "trace.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <pthread.h>

struct __ts_render_arg
{
    int thread_index;
    sem_t *done_signal;
    sem_t thread_signal;

    struct trace_set *ts;
    size_t trace_index;
    int ret;
};

void *__ts_render_func(void *thread_arg)
{
    int ret;
    struct __ts_render_arg *arg = (struct __ts_render_arg *) thread_arg;
    struct trace *trace;

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

        ret = trace_get(arg->ts, &trace, arg->trace_index);
        if(ret < 0)
        {
            err("Thread %i failed to get trace at index %li\n",
                arg->thread_index, arg->trace_index);
            arg->ret = ret;

            sem_release(arg->done_signal);
            return NULL;
        }

        trace_free(trace);

        arg->ret = 1;
        sem_release(arg->done_signal);
    }

    err("Thread %i has finished\n", arg->thread_index);
}

struct render
{
    pthread_t handle;
    int ret;

    struct trace_set *ts;
    size_t nthreads;
};

void *__ts_render_controller(void *controller_arg)
{
    int i, j, ret;
    size_t curr_index = 0;
    sem_t done_signal;

    pthread_t *handles;
    struct __ts_render_arg *args;
    struct render *arg = controller_arg;

    ret = sem_init(&done_signal, 0, arg->nthreads);
    if(ret < 0)
    {
        err("Failed to initialize done semaphore\n");
        arg->ret = -errno;
        return NULL;
    }

    handles = calloc(arg->nthreads, sizeof(pthread_t));
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

        ret = sem_init(&args[i].thread_signal, 0, 0);
        if(ret < 0)
        {
            err("Unable to allocate thread signal for thread %i\n", i);
            ret = -errno;
            goto __teardown_args;
        }
    }

    for(i = 0; i < arg->nthreads; i++)
    {
        ret = pthread_create(&handles[i], NULL, __ts_render_func, &args[i]);
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
                debug("Dispatching index %li to thread %i\n", curr_index, i);

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
        pthread_join(handles[j], NULL);
    }

    i = arg->nthreads;
__teardown_args:
    for(j = 0; j < i; j++)
        sem_destroy(&args[j].thread_signal);
    free(args);

__free_handles:
    free(handles);

__destroy_done_signal:
    sem_destroy(&done_signal);

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

    ret = pthread_create(&res->handle, NULL, __ts_render_controller, res);
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

    pthread_join(render->handle, NULL);
    ret = render->ret;

    free(render);
    return ret;
}