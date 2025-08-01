/*
 * Facility for queueing callback functions to be run from the
 * top-level event loop after the current top-level activity finishes.
 */

#include <stddef.h>

#include "putty.h"

struct callback {
    struct callback *next;

    toplevel_callback_fn_t fn;
    void *ctx;
};

static struct callback *cbcurr = NULL, *cbhead = NULL, *cbtail = NULL;

static toplevel_callback_notify_fn_t notify_frontend = NULL;
static void *notify_ctx = NULL;

void request_callback_notifications(toplevel_callback_notify_fn_t fn,
                                    void *ctx)
{
    notify_frontend = fn;
    notify_ctx = ctx;
}

static void run_idempotent_callback(void *ctx)
{
    struct IdempotentCallback *ic = (struct IdempotentCallback *)ctx;
    ic->queued = false;
    ic->fn(ic->ctx);
}

void queue_idempotent_callback(struct IdempotentCallback *ic)
{
    if (ic->queued)
        return;
    ic->queued = true;
    queue_toplevel_callback(run_idempotent_callback, ic);
}

void delete_callbacks(
    bool (*delete_this_one)(void *predicate_ctx, toplevel_callback_fn_t fn,
                            void *callback_ctx), void *predicate_ctx)
{
    struct callback *newhead, *newtail;

    newhead = newtail = NULL;
    while (cbhead) {
        struct callback *cb = cbhead;
        cbhead = cbhead->next;
        if (delete_this_one(predicate_ctx, cb->fn, cb->ctx)) {
            sfree(cb);
        } else {
            if (!newhead)
                newhead = cb;
            else
                newtail->next = cb;

            newtail = cb;
        }
    }

    cbhead = newhead;
    cbtail = newtail;
    if (newtail)
        newtail->next = NULL;
}

static bool callback_is_for_context(
    void *predicate_ctx, toplevel_callback_fn_t fn, void *callback_ctx)
{
    return callback_ctx == predicate_ctx ||
        (fn == run_idempotent_callback &&
         ((struct IdempotentCallback *)callback_ctx)->ctx == predicate_ctx);
}

void delete_callbacks_for_context(void *ctx)
{
    delete_callbacks(callback_is_for_context, ctx);
}

void queue_toplevel_callback(toplevel_callback_fn_t fn, void *ctx)
{
    struct callback *cb;

    cb = snew(struct callback);
    cb->fn = fn;
    cb->ctx = ctx;

    /*
     * If the front end has requested notification of pending
     * callbacks, and we didn't already have one queued, let it know
     * we do have one now.
     *
     * If cbcurr is non-NULL, i.e. we are actually in the middle of
     * executing a callback right now, then we count that as the queue
     * already having been non-empty. That saves the front end getting
     * a constant stream of needless re-notifications if the last
     * callback keeps re-scheduling itself.
     */
    if (notify_frontend && !cbhead && !cbcurr)
        notify_frontend(notify_ctx);

    if (cbtail)
        cbtail->next = cb;
    else
        cbhead = cb;
    cbtail = cb;
    cb->next = NULL;
}

bool run_toplevel_callbacks(void)
{
    bool done_something = false;

    if (cbhead) {
        /*
         * Transfer the head callback into cbcurr to indicate that
         * it's being executed. Then operations which transform the
         * queue, like delete_callbacks_for_context, can proceed as if
         * it's not there.
         */
        cbcurr = cbhead;
        cbhead = cbhead->next;
        if (!cbhead)
            cbtail = NULL;

        /*
         * Now run the callback, and then clear it out of cbcurr.
         */
        cbcurr->fn(cbcurr->ctx);
        sfree(cbcurr);
        cbcurr = NULL;

        done_something = true;
    }
    return done_something;
}

bool toplevel_callback_pending(void)
{
    return cbcurr != NULL || cbhead != NULL;
}
