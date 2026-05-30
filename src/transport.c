#include "transport.h"

#include "framing.h"
#include "server.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <libubox/list.h>
#include <libubox/uloop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct write_queue_entry_st
{
    struct list_head list;
    char * buf;
    size_t len;
    size_t pos;
} write_queue_entry_st;

static void
check_exit_condition(rpc_server_st * svr)
{
    if (svr->eof_reached && list_empty(&svr->write_queue) && list_empty(&svr->tool_queue.tasks_active.list))
    {
        uloop_end();
    }
}

static void
write_queue_cb(struct uloop_fd * u, unsigned int events)
{
    UNUSED_PARAM(events);
    rpc_server_st * const svr = container_of(u, rpc_server_st, out_uloop_fd);

    while (!list_empty(&svr->write_queue))
    {
        write_queue_entry_st * entry = list_first_entry(&svr->write_queue, write_queue_entry_st, list);

        ssize_t const bytes_written = write(u->fd, entry->buf + entry->pos, entry->len - entry->pos);

        if (bytes_written < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            else if (errno == EINTR)
            {
                continue;
            }
            else
            {
                perror("write_queue_cb: write failed");
                uloop_end();
                return;
            }
        }

        entry->pos += bytes_written;
        if (entry->pos == entry->len)
        {
            list_del(&entry->list);
            free(entry->buf);
            free(entry);
        }
    }

    uloop_fd_delete(&svr->out_uloop_fd);
    check_exit_condition(svr);
}

static bool
append_to_buffer(rpc_server_st * svr, char const * data, size_t data_len)
{
    size_t needed = svr->buf_len + data_len;
    if (needed > svr->buf_cap)
    {
        size_t new_cap = svr->buf_cap == 0 ? 4096 : svr->buf_cap * 2;
        while (new_cap < needed)
        {
            new_cap *= 2;
        }
        char * new_buf = realloc(svr->buf, new_cap);
        if (!new_buf)
        {
            perror("realloc");
            return false;
        }
        svr->buf = new_buf;
        svr->buf_cap = new_cap;
    }
    memcpy(svr->buf + svr->buf_len, data, data_len);
    svr->buf_len = needed;
    svr->buf[svr->buf_len] = '\0';
    return true;
}

static void
stdin_cb(struct uloop_fd * u, unsigned int events)
{
    UNUSED_PARAM(events);
    rpc_server_st * const svr = container_of(u, rpc_server_st, stdin_fd);

    char tmp[4096];
    ssize_t n = read(u->fd, tmp, sizeof(tmp));

    if (n == 0)
    {
        svr->eof_reached = true;
        uloop_fd_delete(u);
        check_exit_condition(svr);
        return;
    }

    if (n < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            uloop_fd_delete(u);
            uloop_end();
        }
        return;
    }

    fprintf(stderr, "[LSP] read %zu bytes from stdin\n", (size_t)n);

    if (!append_to_buffer(svr, tmp, (size_t)n))
    {
        uloop_end();
        return;
    }

    for (;;)
    {
        size_t msg_offset, msg_len;
        frame_decode_result_t r = svr->framing->decode(svr->framing, svr->buf, svr->buf_len, &msg_offset, &msg_len);

        if (r == FRAME_NEED_MORE)
        {
            break;
        }

        if (r == FRAME_ERROR)
        {
            uloop_end();
            return;
        }

        fprintf(stderr, "[LSP] decoded %zu-byte frame\n", msg_len);

        char saved = svr->buf[msg_offset + msg_len];
        svr->buf[msg_offset + msg_len] = '\0';
        svr->on_transport_msg(svr->buf + msg_offset, msg_len, svr->on_transport_msg_data);
        svr->buf[msg_offset + msg_len] = saved;

        size_t const consumed = msg_offset + msg_len;
        size_t const remaining = svr->buf_len - consumed;
        memmove(svr->buf, svr->buf + consumed, remaining);
        svr->buf_len = remaining;
        svr->buf[svr->buf_len] = '\0';
    }
}

void
transport_init(rpc_server_st * svr)
{
    int flags = fcntl(svr->out_fd, F_GETFL, 0);
    fcntl(svr->out_fd, F_SETFL, flags | O_NONBLOCK);

    INIT_LIST_HEAD(&svr->write_queue);

    svr->stdin_fd.fd = svr->in_fd;
    svr->stdin_fd.cb = stdin_cb;
    uloop_fd_add(&svr->stdin_fd, ULOOP_READ);

    svr->out_uloop_fd.fd = svr->out_fd;
    svr->out_uloop_fd.cb = write_queue_cb;
}

void
transport_cleanup(rpc_server_st * svr)
{
    free(svr->buf);
    svr->buf = NULL;
    svr->buf_len = 0;
    svr->buf_cap = 0;
}

void
transport_send(rpc_server_st * svr, char const * data, size_t len)
{
    fprintf(stderr, "[LSP] queued %zu bytes for send\n", len);

    write_queue_entry_st * entry = malloc(sizeof(*entry));
    entry->len = len;
    entry->buf = malloc(len);
    memcpy(entry->buf, data, len);
    entry->pos = 0;

    bool const was_empty = list_empty(&svr->write_queue);
    list_add_tail(&entry->list, &svr->write_queue);

    if (was_empty)
    {
        uloop_fd_add(&svr->out_uloop_fd, ULOOP_WRITE);
        write_queue_cb(&svr->out_uloop_fd, ULOOP_WRITE);
    }
}

void
transport_close_stdin(rpc_server_st * svr)
{
    uloop_fd_delete(&svr->stdin_fd);
    svr->eof_reached = true;
}

bool
transport_can_exit(rpc_server_st * svr)
{
    return svr->eof_reached && list_empty(&svr->write_queue);
}
