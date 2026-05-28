#include "server.h"

#include "documents.h"
#include "handlers.h"
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
    if (svr->eof_reached && list_empty(&svr->write_queue)
        && list_empty(&svr->tool_queue.tasks_active.list))
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

void
rpc_server_queue_response(rpc_server_st * svr, struct json_object * res)
{
    char const * json_str = json_object_to_json_string_ext(res, JSON_C_TO_STRING_PLAIN);
    size_t const json_len = strlen(json_str);

    char header[64];
    int hdr_len = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", json_len);

    write_queue_entry_st * entry = malloc(sizeof(*entry));
    entry->len = (size_t)hdr_len + json_len;
    entry->buf = malloc(entry->len);
    memcpy(entry->buf, header, (size_t)hdr_len);
    memcpy(entry->buf + (size_t)hdr_len, json_str, json_len);
    entry->pos = 0;

    bool const was_empty = list_empty(&svr->write_queue);
    list_add_tail(&entry->list, &svr->write_queue);

    if (was_empty)
    {
        uloop_fd_add(&svr->out_uloop_fd, ULOOP_WRITE);
        write_queue_cb(&svr->out_uloop_fd, ULOOP_WRITE);
    }
}

static void
rpc_server_registry_cleanup(rpc_server_st * svr)
{
    for (size_t i = 0; i < svr->registry.count; i++)
    {
        free(svr->registry.methods[i].name);
    }
    free(svr->registry.methods);
    svr->registry.count = 0;
    svr->registry.capacity = 0;
}

void
rpc_server_register_method(rpc_server_st * svr, char const * name, rpc_handler_fn handler)
{
    if (svr->registry.count == svr->registry.capacity)
    {
        svr->registry.capacity = svr->registry.capacity == 0 ? 8 : svr->registry.capacity * 2;
        svr->registry.methods = realloc(svr->registry.methods, svr->registry.capacity * sizeof(*svr->registry.methods));
    }
    svr->registry.methods[svr->registry.count].name = strdup(name);
    svr->registry.methods[svr->registry.count].handler = handler;
    svr->registry.count++;
}

static void
queue_error_response(rpc_server_st * svr, struct json_object * id, int code, char const * message)
{
    struct json_object * res = json_object_new_object();
    json_object_object_add(res, "jsonrpc", json_object_new_string("2.0"));

    struct json_object * error = json_object_new_object();
    json_object_object_add(error, "code", json_object_new_int(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(res, "error", error);

    if (id)
    {
        json_object_object_add(res, "id", json_object_get(id));
    }
    else
    {
        json_object_object_add(res, "id", NULL);
    }

    rpc_server_queue_response(svr, res);
    json_object_put(res);
}

static void
handle_rpc_message(rpc_server_st * svr, struct json_object * msg)
{
    struct json_object * id = NULL;
    struct json_object * method_obj = NULL;
    struct json_object * params = NULL;
    struct json_object * version = NULL;

    if (!json_object_is_type(msg, json_type_object))
    {
        fprintf(stderr, "[LSP] Error: message is not a JSON object\n");
        return;
    }

    if (!json_object_object_get_ex(msg, "jsonrpc", &version) || strcmp(json_object_get_string(version), "2.0") != 0)
    {
        fprintf(stderr, "[LSP] Error: invalid JSON-RPC version\n");
        return;
    }

    json_object_object_get_ex(msg, "id", &id);

    if (!json_object_object_get_ex(msg, "method", &method_obj) || !json_object_is_type(method_obj, json_type_string))
    {
        fprintf(stderr, "[LSP] Error: invalid method\n");
        if (id)
        {
            queue_error_response(svr, id, -32600, "Invalid Request");
        }
        return;
    }

    char const * method_name = json_object_get_string(method_obj);

    json_object_object_get_ex(msg, "params", &params);

    rpc_handler_fn handler = NULL;
    for (size_t i = 0; i < svr->registry.count; i++)
    {
        if (strcmp(svr->registry.methods[i].name, method_name) == 0)
        {
            handler = svr->registry.methods[i].handler;
            break;
        }
    }

    if (handler)
    {
        if (!handler(svr, params, id))
        {
            fprintf(stderr, "[LSP] Error: failed to handle method '%s'\n", method_name);
            queue_error_response(svr, id, -32600, "Invalid Request");
        }
    }
    else if (id)
    {
        fprintf(stderr, "[LSP] Error: method '%s' not found\n", method_name);
        queue_error_response(svr, id, -32601, "Method not found");
    }
    else
    {
        fprintf(stderr, "[LSP] Error: method '%s' not found (notification)\n", method_name);
    }
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

    if (u->eof)
    {
        svr->eof_reached = true;
        uloop_fd_delete(u);
        check_exit_condition(svr);
        return;
    }

    if (u->error)
    {
        uloop_fd_delete(u);
        uloop_end();
        return;
    }

    /* Read into a local stack buffer, then append to the dynamic buffer. */
    char tmp[4096];
    ssize_t n = read(u->fd, tmp, sizeof(tmp));

    if (n <= 0)
    {
        if (n == 0)
        {
            svr->eof_reached = true;
            uloop_fd_delete(u);
            check_exit_condition(svr);
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            uloop_fd_delete(u);
            uloop_end();
        }
        return;
    }

    if (!append_to_buffer(svr, tmp, (size_t)n))
    {
        uloop_end();
        return;
    }
    fprintf(stderr, "[LSP] read %zd bytes (buf_len=%zu)\n", n, svr->buf_len);

    while (1)
    {
        if (svr->in_header)
        {
            /* Look for the Content-Length header. */
            char * content_len_str = strstr(svr->buf, "Content-Length: ");

            if (content_len_str == NULL)
            {
                /* No header found yet. Error if we see the blank line marker */
                if (strstr(svr->buf, "\r\n\r\n"))
                {
                    fprintf(stderr, "Error: missing Content-Length header\n");
                    uloop_end();
                    return;
                }
                break;
            }

            if (sscanf(content_len_str, "Content-Length: %d", &svr->content_length) != 1
                || svr->content_length < 0)
            {
                fprintf(stderr, "Error: invalid Content-Length\n");
                uloop_end();
                return;
            }

            /* Find the blank line that ends the header section. */
            char * header_end = strstr(svr->buf, "\r\n\r\n");
            if (header_end == NULL)
            {
                break;
            }

            size_t const header_consumed = (size_t)(header_end - svr->buf) + 4;
            size_t const remaining = svr->buf_len - header_consumed;
            memmove(svr->buf, svr->buf + header_consumed, remaining);
            svr->buf_len = remaining;
            svr->buf[svr->buf_len] = '\0';
            svr->in_header = false;
        }
        else
        {
            /* Reading body. */
            if (svr->buf_len < (size_t)svr->content_length)
            {
                break;
            }

            char saved = svr->buf[svr->content_length];
            svr->buf[svr->content_length] = '\0';

            struct json_object * msg = json_tokener_parse(svr->buf);
            if (msg != NULL)
            {
                handle_rpc_message(svr, msg);
                json_object_put(msg);
            }
            else
            {
                fprintf(stderr, "Error: failed to parse JSON body\n");
            }

            svr->buf[svr->content_length] = saved;

            size_t const remaining = svr->buf_len - (size_t)svr->content_length;
            memmove(svr->buf, svr->buf + svr->content_length, remaining);
            svr->buf_len = remaining;
            svr->buf[svr->buf_len] = '\0';
            svr->content_length = -1;
            svr->in_header = true;
        }
    }
}

void
run_server(rpc_server_st * const svr, int const in_fd, int const out_fd)
{
    fprintf(stderr, "[LSP] Server starting on in_fd=%d, out_fd=%d\n", in_fd, out_fd);
    svr->out_fd = out_fd;
    svr->in_fd = in_fd;
    svr->in_header = true;
    svr->content_length = -1;

    int flags = fcntl(out_fd, F_GETFL, 0);
    fcntl(out_fd, F_SETFL, flags | O_NONBLOCK);

    documents_init();

    uloop_init();
    INIT_LIST_HEAD(&svr->write_queue);
    runqueue_init(&svr->tool_queue);
    svr->tool_queue.max_running_tasks = 4;

    rpc_server_register_handlers(svr);

    svr->stdin_fd.fd = svr->in_fd;
    svr->stdin_fd.cb = stdin_cb;
    uloop_fd_add(&svr->stdin_fd, ULOOP_READ);

    svr->out_uloop_fd.fd = svr->out_fd;
    svr->out_uloop_fd.cb = write_queue_cb;

    uloop_run();

    rpc_server_registry_cleanup(svr);
    runqueue_kill(&svr->tool_queue);
    documents_cleanup();
    free(svr->buf);
    svr->buf = NULL;
    uloop_done();

    if (in_fd != STDIN_FILENO)
    {
        close(in_fd);
    }
}
