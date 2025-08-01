#include "putty.h"

struct select_result_params {
    /* Used to pass data to select_result_callback */
    WPARAM wParam;
    LPARAM lParam;
};

static void select_result_callback(void *vctx)
{
    struct select_result_params *params = (struct select_result_params *)vctx;
    select_result(params->wParam, params->lParam);
    sfree(params);
}

static bool callback_is_for_socket(
    void *predicate_ctx, toplevel_callback_fn_t fn, void *callback_ctx)
{
    if (fn != select_result_callback)
        return false;
    struct select_result_params *params =
        (struct select_result_params *)callback_ctx;
    if (params->wParam != (WPARAM)(uintptr_t)predicate_ctx)
        return false;

    /* The 'struct select_result_params' would have been freed by the
     * callback function select_result_callback(). Now that isn't going
     * to run, so we must free it ourself. */
    sfree(callback_ctx);
    return true;
}

void done_with_socket(SOCKET skt)
{
    delete_callbacks(callback_is_for_socket, (void *)(uintptr_t)skt);
}

void cli_main_loop(cliloop_pre_t pre, cliloop_post_t post, void *ctx)
{
    SOCKET *sklist = NULL;
    size_t skcount = 0, sksize = 0;
    unsigned long now, next, then;
    now = GETTICKCOUNT();

    while (true) {
        DWORD n;
        DWORD ticks;

        const HANDLE *extra_handles = NULL;
        size_t n_extra_handles = 0;
        if (!pre(ctx, &extra_handles, &n_extra_handles))
            break;

        if (toplevel_callback_pending()) {
            ticks = 0;
            next = now;
        } else if (run_timers(now, &next)) {
            then = now;
            now = GETTICKCOUNT();
            if (now - then > next - then)
                ticks = 0;
            else
                ticks = next - now;
        } else {
            ticks = INFINITE;
            /* no need to initialise next here because we can never
             * get WAIT_TIMEOUT */
        }

        HandleWaitList *hwl = get_handle_wait_list();
        size_t winselcli_index = -(size_t)1;
        size_t extra_base = hwl->nhandles;
        if (winselcli_event != INVALID_HANDLE_VALUE) {
            assert(extra_base < MAXIMUM_WAIT_OBJECTS);
            winselcli_index = extra_base++;
            hwl->handles[winselcli_index] = winselcli_event;
        }
        size_t total_handles = extra_base + n_extra_handles;
        assert(total_handles < MAXIMUM_WAIT_OBJECTS);
        for (size_t i = 0; i < n_extra_handles; i++)
            hwl->handles[extra_base + i] = extra_handles[i];

        n = WaitForMultipleObjects(total_handles, hwl->handles, false, ticks);

        size_t extra_handle_index = n_extra_handles;

        if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)hwl->nhandles) {
            handle_wait_activate(hwl, n - WAIT_OBJECT_0);
        } else if (winselcli_event != INVALID_HANDLE_VALUE &&
                   n == WAIT_OBJECT_0 + winselcli_index) {
            WSANETWORKEVENTS things;
            SOCKET socket;
            int i, socketstate;

            /*
             * We must not call select_result() for any socket
             * until we have finished enumerating within the tree.
             * This is because select_result() may close the socket
             * and modify the tree.
             */
            /* Count the active sockets. */
            i = 0;
            for (socket = first_socket(&socketstate);
                 socket != INVALID_SOCKET;
                 socket = next_socket(&socketstate)) i++;

            /* Expand the buffer if necessary. */
            sgrowarray(sklist, sksize, i);

            /* Retrieve the sockets into sklist. */
            skcount = 0;
            for (socket = first_socket(&socketstate);
                 socket != INVALID_SOCKET;
                 socket = next_socket(&socketstate)) {
                sklist[skcount++] = socket;
            }

            /* Now we're done enumerating; go through the list. */
            for (i = 0; i < skcount; i++) {
                socket = sklist[i];
                if (!p_WSAEnumNetworkEvents(socket, NULL, &things)) {
                    static const struct { int bit, mask; } eventtypes[] = {
                        {FD_CONNECT_BIT, FD_CONNECT},
                        {FD_READ_BIT, FD_READ},
                        {FD_CLOSE_BIT, FD_CLOSE},
                        {FD_OOB_BIT, FD_OOB},
                        {FD_WRITE_BIT, FD_WRITE},
                        {FD_ACCEPT_BIT, FD_ACCEPT},
                    };
                    int e;

                    noise_ultralight(NOISE_SOURCE_IOID, socket);

                    for (e = 0; e < lenof(eventtypes); e++)
                        if (things.lNetworkEvents & eventtypes[e].mask) {
                            int err = things.iErrorCode[eventtypes[e].bit];
                            struct select_result_params *params =
                                snew(struct select_result_params);
                            params->wParam = (WPARAM)socket;
                            params->lParam = WSAMAKESELECTREPLY(
                                eventtypes[e].mask, err);
                            queue_toplevel_callback(
                                select_result_callback, params);
                        }
                }
            }
        } else if (n >= WAIT_OBJECT_0 + extra_base &&
                   n < WAIT_OBJECT_0 + extra_base + n_extra_handles) {
            extra_handle_index = n - (WAIT_OBJECT_0 + extra_base);
        }

        run_toplevel_callbacks();

        if (n == WAIT_TIMEOUT) {
            now = next;
        } else {
            now = GETTICKCOUNT();
        }

        handle_wait_list_free(hwl);

        if (!post(ctx, extra_handle_index))
            break;
    }

    sfree(sklist);
}

bool cliloop_null_pre(void *vctx, const HANDLE **eh, size_t *neh)
{ return true; }
bool cliloop_null_post(void *vctx, size_t ehi) { return true; }
