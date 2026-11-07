#ifndef LUMEN_LIST_H
#define LUMEN_LIST_H

/*
 * Header-only intrusive doubly linked list and signal/listener helpers.
 * Written and owned by the architect; fully implemented here.
 */

#include <stdbool.h>
#include <stddef.h>

#define lm_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

struct lm_list {
    struct lm_list *prev;
    struct lm_list *next;
};

/* A list head or a detached element must be initialised before use. */
static inline void lm_list_init(struct lm_list *l)
{
    l->prev = l;
    l->next = l;
}

/* Insert `elm` right after `pos` (pos may be the list head). */
static inline void lm_list_insert(struct lm_list *pos, struct lm_list *elm)
{
    elm->prev = pos;
    elm->next = pos->next;
    pos->next->prev = elm;
    pos->next = elm;
}

/* Add at the tail (before the head). */
static inline void lm_list_append(struct lm_list *head, struct lm_list *elm)
{
    lm_list_insert(head->prev, elm);
}

/* Unlink and re-initialise: calling it twice on the same element is safe. */
static inline void lm_list_remove(struct lm_list *elm)
{
    elm->prev->next = elm->next;
    elm->next->prev = elm->prev;
    lm_list_init(elm);
}

static inline bool lm_list_empty(const struct lm_list *head)
{
    return head->next == head;
}

/* True if the element is currently linked into some list. */
static inline bool lm_list_linked(const struct lm_list *elm)
{
    return elm->next != elm;
}

static inline int lm_list_length(const struct lm_list *head)
{
    int n = 0;
    for (const struct lm_list *p = head->next; p != head; p = p->next)
        n++;
    return n;
}

/* Move all elements of `from` to the tail of `to`; `from` becomes empty. */
static inline void lm_list_splice(struct lm_list *to, struct lm_list *from)
{
    if (lm_list_empty(from))
        return;
    struct lm_list *first = from->next, *last = from->prev;
    first->prev = to->prev;
    to->prev->next = first;
    last->next = to;
    to->prev = last;
    lm_list_init(from);
}

#define lm_list_for_each(pos, head, member) \
    for (pos = lm_container_of((head)->next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = lm_container_of(pos->member.next, __typeof__(*pos), member))

/* Safe against removal of `pos` inside the loop body. */
#define lm_list_for_each_safe(pos, tmp, head, member) \
    for (pos = lm_container_of((head)->next, __typeof__(*pos), member), \
         tmp = lm_container_of(pos->member.next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = tmp, \
         tmp = lm_container_of(pos->member.next, __typeof__(*pos), member))

#define lm_list_for_each_reverse(pos, head, member) \
    for (pos = lm_container_of((head)->prev, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = lm_container_of(pos->member.prev, __typeof__(*pos), member))

/* ---- signals ---------------------------------------------------------- */

struct lm_listener;
typedef void (*lm_notify_fn)(struct lm_listener *listener, void *data);

struct lm_listener {
    struct lm_list link;
    lm_notify_fn notify;
};

struct lm_signal {
    struct lm_list listeners;
};

static inline void lm_signal_init(struct lm_signal *s)
{
    lm_list_init(&s->listeners);
}

static inline void lm_signal_add(struct lm_signal *s, struct lm_listener *l)
{
    lm_list_append(&s->listeners, &l->link);
}

/* Safe to call on a listener that was never added (init it with lm_list_init). */
static inline void lm_listener_remove(struct lm_listener *l)
{
    lm_list_remove(&l->link);
}

/*
 * Call every listener once. A notify callback may remove itself or any other
 * listener, or free its own listener struct. Listeners added during emission
 * are also called in this emission.
 */
static inline void lm_signal_emit(struct lm_signal *s, void *data)
{
    struct lm_list done;
    lm_list_init(&done);
    while (!lm_list_empty(&s->listeners)) {
        struct lm_listener *l =
            lm_container_of(s->listeners.next, struct lm_listener, link);
        lm_list_remove(&l->link);
        lm_list_append(&done, &l->link);
        l->notify(l, data);
    }
    lm_list_splice(&s->listeners, &done);
}

#endif
