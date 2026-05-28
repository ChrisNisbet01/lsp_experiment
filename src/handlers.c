#include "handlers.h"

#include "documents.h"
#include "server.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <libubox/runqueue.h>
#include <libubox/uloop.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Path to the cyclomatic_complexity tool binary.
 * Adjust this to match your installation.
 */
#define CYCLOMATIC_COMPLEXITY_PATH                                    \
    "/home/chris/projects/c_tools/cyclomatic_complexity/build/"       \
    "cyclomatic_complexity"

/* --- Helpers --- */

static void
queue_success_response(rpc_server_st * svr, struct json_object * id, struct json_object * result)
{
    struct json_object * res = json_object_new_object();
    json_object_object_add(res, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(res, "result", result);
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

/* --- Lifecycle --- */

static bool
handle_initialize(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)params;

    struct json_object * result = json_object_new_object();

    struct json_object * capabilities = json_object_new_object();
    json_object_object_add(capabilities, "textDocumentSync", json_object_new_int(1));
    json_object_object_add(result, "capabilities", capabilities);

    queue_success_response(svr, id, result);

    return true;
}

static bool
handle_shutdown(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)params;

    svr->shutdown_requested = true;

    queue_success_response(svr, id, NULL);

    return true;
}

static bool
handle_exit(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    UNUSED_PARAM(params);
    UNUSED_PARAM(id);

    uloop_fd_delete(&svr->stdin_fd);
    svr->eof_reached = true;

    if (list_empty(&svr->write_queue) && list_empty(&svr->tool_queue.tasks_active.list))
    {
        uloop_end();
    }

    return true;
}

/* --- Document synchronization --- */

static bool
handle_text_document_did_open(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)svr;
    (void)id;

    struct json_object * text_document = NULL;
    if (!json_object_object_get_ex(params, "textDocument", &text_document))
    {
        fprintf(stderr, "[LSP] Error: 'textDocument' field missing in params\n");
        return false;
    }

    struct json_object * uri_obj = NULL;
    struct json_object * text_obj = NULL;
    if (!json_object_object_get_ex(text_document, "uri", &uri_obj)
        || !json_object_object_get_ex(text_document, "text", &text_obj))
    {
        fprintf(stderr, "[LSP] Error: missing required fields in 'textDocument'\n");
        return false;
    }

    documents_update(json_object_get_string(uri_obj), json_object_get_string(text_obj));

    return true;
}

static bool
handle_text_document_did_change(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)svr;
    (void)id;

    struct json_object * text_document = NULL;
    if (!json_object_object_get_ex(params, "textDocument", &text_document))
    {
        return false;
    }

    struct json_object * uri_obj = NULL;
    if (!json_object_object_get_ex(text_document, "uri", &uri_obj))
    {
        return false;
    }

    struct json_object * content_changes = NULL;
    if (!json_object_object_get_ex(params, "contentChanges", &content_changes)
        || json_object_array_length(content_changes) < 1)
    {
        return false;
    }

    struct json_object * change = json_object_array_get_idx(content_changes, 0);
    struct json_object * text_obj = NULL;
    if (!json_object_object_get_ex(change, "text", &text_obj))
    {
        return false;
    }

    documents_update(json_object_get_string(uri_obj), json_object_get_string(text_obj));

    return true;
}

static bool
handle_text_document_did_close(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)svr;
    (void)id;

    struct json_object * text_document = NULL;
    if (!json_object_object_get_ex(params, "textDocument", &text_document))
    {
        return false;
    }

    struct json_object * uri_obj = NULL;
    if (!json_object_object_get_ex(text_document, "uri", &uri_obj))
    {
        return false;
    }

    documents_remove(json_object_get_string(uri_obj));

    return true;
}

/* --- Function Complexity feature --- */

typedef struct complexity_context_st
{
    rpc_server_st * svr;
    struct runqueue_process run_proc;
    struct uloop_fd pipe_fd;
    struct json_object * id;
    struct json_object * params;
    char * output;
    size_t output_len;
    char * temp_path;
} complexity_context_st;

static void
complexity_pipe_cb(struct uloop_fd * u, unsigned int events)
{
    UNUSED_PARAM(events);
    complexity_context_st * ctx = container_of(u, complexity_context_st, pipe_fd);
    char buf[4096];
    ssize_t n;

    while ((n = read(u->fd, buf, sizeof(buf))) > 0)
    {
        char * new_output = realloc(ctx->output, ctx->output_len + (size_t)n + 1);
        if (!new_output)
        {
            perror("realloc");
            return;
        }
        ctx->output = new_output;
        memcpy(ctx->output + ctx->output_len, buf, (size_t)n);
        ctx->output_len += (size_t)n;
        ctx->output[ctx->output_len] = '\0';
    }

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        uloop_fd_delete(u);
        close(u->fd);
        u->fd = -1;
    }
}

static void
complexity_task_complete_cb(struct runqueue * q, struct runqueue_task * t)
{
    (void)q;
    complexity_context_st * ctx = container_of(t, complexity_context_st, run_proc.task);

    /* Final read to capture any remaining output. */
    if (ctx->pipe_fd.fd != -1)
    {
        complexity_pipe_cb(&ctx->pipe_fd, ULOOP_READ);

        if (ctx->pipe_fd.fd != -1)
        {
            uloop_fd_delete(&ctx->pipe_fd);
            close(ctx->pipe_fd.fd);
            ctx->pipe_fd.fd = -1;
        }
    }

    /* Parse the JSON output from the complexity tool. */
    int complexity_value = -1;
    char func_name_buf[256] = "";

    if (ctx->output && ctx->output[0])
    {
        fprintf(stderr, "[LSP] complexity output: %s\n", ctx->output);
        struct json_object * arr = json_tokener_parse(ctx->output);

        if (arr && json_object_is_type(arr, json_type_array))
        {
            int const len = json_object_array_length(arr);
            if (len > 0)
            {
                struct json_object * item = json_object_array_get_idx(arr, 0);
                struct json_object * comp_obj = NULL;
                struct json_object * name_obj = NULL;

                if (json_object_object_get_ex(item, "complexity", &comp_obj))
                {
                    complexity_value = json_object_get_int(comp_obj);
                }
                if (json_object_object_get_ex(item, "function_name", &name_obj))
                {
                    snprintf(
                        func_name_buf, sizeof(func_name_buf), "%s",
                        json_object_get_string(name_obj)
                    );
                }
            }
        }
        json_object_put(arr);
    }

    /* Build and queue the response. */
    struct json_object * result = json_object_new_object();
    json_object_object_add(result, "complexity", json_object_new_int(complexity_value));
    if (func_name_buf[0])
    {
        json_object_object_add(result, "function_name", json_object_new_string(func_name_buf));
    }

    queue_success_response(ctx->svr, ctx->id, result);

    /* Clean up temp file. */
    if (ctx->temp_path)
    {
        unlink(ctx->temp_path);
        free(ctx->temp_path);
    }

    json_object_put(ctx->id);
    json_object_put(ctx->params);
    free(ctx->output);
    free(ctx);
}

static void
complexity_run_cb(struct runqueue * q, struct runqueue_task * t)
{
    complexity_context_st * ctx = container_of(t, complexity_context_st, run_proc.task);

    /* Extract URI and function name from the stored params. */
    struct json_object * text_document_obj = NULL;
    struct json_object * func_name_obj = NULL;

    json_object_object_get_ex(ctx->params, "textDocument", &text_document_obj);
    json_object_object_get_ex(ctx->params, "functionName", &func_name_obj);

    if (!text_document_obj || !func_name_obj)
    {
        fprintf(stderr, "[LSP] complexity: missing textDocument or functionName\n");
        runqueue_task_complete(t);
        return;
    }

    struct json_object * uri_obj = NULL;
    json_object_object_get_ex(text_document_obj, "uri", &uri_obj);
    char const * uri = json_object_get_string(uri_obj);
    char const * function_name = json_object_get_string(func_name_obj);

    fprintf(
        stderr, "[LSP] complexity: uri=%s, function=%s\n",
        uri ? uri : "(null)", function_name ? function_name : "(null)"
    );

    if (!uri || !function_name)
    {
        runqueue_task_complete(t);
        return;
    }

    /* Look up the document text from the in-memory store. */
    document_st * doc = documents_lookup(uri);
    if (!doc)
    {
        fprintf(stderr, "[LSP] complexity: document not found: %s\n", uri);
        runqueue_task_complete(t);
        return;
    }

    /* Write document text to a temporary file. */
    char tmp_pattern[] = "/tmp/c_tools_XXXXXX";
    int tmp_fd = mkstemp(tmp_pattern);
    if (tmp_fd < 0)
    {
        perror("mkstemp");
        runqueue_task_complete(t);
        return;
    }

    size_t text_len = strlen(doc->text);
    ssize_t written = write(tmp_fd, doc->text, text_len);
    if (written < 0 || (size_t)written != text_len)
    {
        perror("write");
        close(tmp_fd);
        unlink(tmp_pattern);
        runqueue_task_complete(t);
        return;
    }
    close(tmp_fd);

    ctx->temp_path = strdup(tmp_pattern);
    fprintf(stderr, "[LSP] complexity: wrote temp file %s\n", ctx->temp_path);

    /* Create a pipe to capture child stdout. */
    int pipefds[2];
    if (pipe(pipefds) < 0)
    {
        perror("pipe");
        unlink(ctx->temp_path);
        free(ctx->temp_path);
        ctx->temp_path = NULL;
        runqueue_task_complete(t);
        return;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        close(pipefds[0]);
        close(pipefds[1]);
        unlink(ctx->temp_path);
        free(ctx->temp_path);
        ctx->temp_path = NULL;
        runqueue_task_complete(t);
        return;
    }

    if (pid == 0)
    {
        /* Child process: redirect stdout to the pipe and exec the tool. */
        close(pipefds[0]);

        if (dup2(pipefds[1], STDOUT_FILENO) < 0)
        {
            _exit(127);
        }
        close(pipefds[1]);

        char * const argv[] = {
            "cyclomatic_complexity",
            "--no-preprocess",
            "-j",
            "-f",
            (char *)function_name,
            (char *)ctx->temp_path,
            NULL
        };

        execv(CYCLOMATIC_COMPLEXITY_PATH, argv);
        perror("execv failed");
        _exit(127);
    }

    /* Parent process. */
    close(pipefds[1]);

    int flags = fcntl(pipefds[0], F_GETFL, 0);
    fcntl(pipefds[0], F_SETFL, flags | O_NONBLOCK);

    ctx->pipe_fd.fd = pipefds[0];
    uloop_fd_add(&ctx->pipe_fd, ULOOP_READ);

    runqueue_process_add(q, &ctx->run_proc, pid);
}

static void
complexity_cancel_cb(struct runqueue * q, struct runqueue_task * t, int type)
{
    runqueue_process_cancel_cb(q, t, type);
}

static const struct runqueue_task_type complexity_task_type = {
    .run = complexity_run_cb,
    .cancel = complexity_cancel_cb,
    .kill = runqueue_process_kill_cb,
};

static bool
handle_function_complexity(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    struct json_object * text_document_obj = NULL;
    struct json_object * func_name_obj = NULL;

    if (!json_object_object_get_ex(params, "textDocument", &text_document_obj)
        || !json_object_object_get_ex(params, "functionName", &func_name_obj))
    {
        fprintf(stderr, "[LSP] complexity request missing textDocument or functionName\n");
        return false;
    }

    if (!json_object_is_type(text_document_obj, json_type_object)
        || !json_object_is_type(func_name_obj, json_type_string))
    {
        fprintf(stderr, "[LSP] complexity request: invalid parameter types\n");
        return false;
    }

    complexity_context_st * ctx = calloc(1, sizeof(*ctx));
    ctx->svr = svr;
    ctx->id = id ? json_object_get(id) : NULL;
    ctx->params = params ? json_object_get(params) : NULL;
    ctx->pipe_fd.cb = complexity_pipe_cb;
    ctx->pipe_fd.fd = -1;
    ctx->run_proc.task.type = &complexity_task_type;
    ctx->run_proc.task.complete = complexity_task_complete_cb;

    runqueue_task_add(&svr->tool_queue, &ctx->run_proc.task, false);

    return true;
}

/* --- No-op handlers for standard LSP notifications --- */

static bool
handle_initialized(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    UNUSED_PARAM(svr);
    UNUSED_PARAM(params);
    UNUSED_PARAM(id);
    return true;
}

/* --- Registration --- */

void
rpc_server_register_handlers(rpc_server_st * svr)
{
    rpc_server_register_method(svr, "initialize", handle_initialize);
    rpc_server_register_method(svr, "initialized", handle_initialized);
    rpc_server_register_method(svr, "shutdown", handle_shutdown);
    rpc_server_register_method(svr, "exit", handle_exit);
    rpc_server_register_method(svr, "textDocument/didOpen", handle_text_document_did_open);
    rpc_server_register_method(svr, "textDocument/didChange", handle_text_document_did_change);
    rpc_server_register_method(svr, "textDocument/didClose", handle_text_document_did_close);
    rpc_server_register_method(svr, "textDocument/functionComplexity", handle_function_complexity);
}
