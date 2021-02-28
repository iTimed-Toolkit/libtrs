#include "__libtrs_internal.h"

#include <stdlib.h>
#include <errno.h>

struct list
{
    struct list *prev, *next;
    void *data;
};

int list_create_node(struct list **node, void *data)
{
    struct list *res;
    if(!node)
    {
        err("Invalid pointer to result destination\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct list));
    res->data = data;

    *node = res;
    return 0;
}

int list_free_node(struct list *node)
{
    if(!node)
    {
        err("Invalid node\n");
        return -EINVAL;
    }

    if(node->prev || node->next)
    {
        err("Node seems to still be linked to a list\n");
        return -EINVAL;
    }

    free(node);
    return 0;
}

int list_link_single(struct list **head,
                     struct list *node,
                     list_comparison_t comp)
{
    int i = 0;
    struct list *curr, *prev = NULL;
    if(!head || !node || !comp)
    {
        err("Invalid list head, node, or comparison function\n");
        return -EINVAL;
    }

    debug("Linking node at %p\n", node);
    if(*head == NULL)
    {
        debug("Setting head\n");
        *head = node;
        return 0;
    }

    if(comp(node->data, (*head)->data) > 0)
    {
        debug("Replacing head\n");
        node->next = *head;
        *head = node;
        return 0;
    }

    for(curr = (*head)->next, prev = *head;
        curr != NULL;
        prev = curr, curr = curr->next)
    {
        if(comp(prev->data, node->data) < 0 &&
           comp(node->data, curr->data) > 0)
        {
            debug("Placing at index %i, prev = %p curr = %p\n", i, prev, curr);
            prev->next = node;
            node->next = curr;
            return 0;
        }

        i++;
    }

    debug("Placing at index %i (end), prev = %p\n", i, prev);
    prev->next = node;
    return 0;
}

int list_unlink_single(struct list **head, struct list *node)
{
    int i = 0;
    struct list *curr;

    if(!head || !node)
    {
        err("Invalid list head or node\n");
        return -EINVAL;
    }

    debug("Unlinking node %p\n", node);
    if(*head == node)
    {
        debug("Removing head\n");
        *head = node->next;
        node->next = NULL;
        return 0;
    }

    for(curr = *head; curr != NULL; curr = curr->next)
    {
        if(curr->next == node)
        {
            debug("Removing at index %i, curr = %p\n", i, curr);
            curr->next = node->next;
            node->next = NULL;
            return 0;
        }

        i++;
    }

    err("Node not found in this list\n");
    return -EINVAL;
}

int list_lookup_single(struct list *head, struct list *node)
{
    int i = 0;
    struct list *curr;

    if(!head || !node)
    {
        err("Invalid list head or node");
        return -EINVAL;
    }

    debug("Looking up node %p\n", node);
    for(curr = head; curr != NULL; curr = curr->next, i++)
    {
        if(curr == node)
            return i;
    }

    err("Node %p not found in this list\n", node);
    return -EINVAL;
}

void *list_get_data(struct list *node)
{
    if(!node)
    {
        err("Invalid node specified\n");
        return NULL;
    }

    return node->data;
}

int list_dump(struct list *head, list_print_t f)
{
    int i = 0;
    struct list *curr;

    debug("List dump:\n");
    for(curr = head; curr != NULL; curr = curr->next, i++)
    {
        debug("\tList index %i (%p):", i, curr);
        f(curr->data);
    }

    if(i == 0)
        debug("\t[empty]\n");

    return 0;
}
