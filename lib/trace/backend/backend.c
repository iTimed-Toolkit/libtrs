#include "__trace_internal.h"
#include "__backend_internal.h"

int create_backend(struct trace_set *ts, const char *name)
{
    char *tok, **pos = (char **) &name;
    tok = strsep(pos, " ");

    if(strcmp(tok, "trs") == 0)
        return create_backend_trs(ts, *pos);
    else if(strcmp(tok, "ztrs") == 0)
        return create_backend_ztrs(ts, *pos);
    else if(strcmp(tok, "net") == 0)
        return create_backend_net(ts, *pos);
    else
    {
        err("Couldn't find a backend for %s\n", tok);
        return -EINVAL;
    }
}