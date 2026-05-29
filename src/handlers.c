#include "handlers.h"

#include "documents.h"
#include "rpc.h"
#include "server.h"
#include "tool_runner.h"
#include "transport.h"
#include "utils.h"

#include <json-c/json.h>
#include <libubox/runqueue.h>
#include <libubox/uloop.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CYCLOMATIC_COMPLEXITY_PATH                                                                                     \
    "/home/chris/projects/c_tools/cyclomatic_complexity/"                                                              \
    "build/cyclomatic_complexity"

/* --- Lifecycle --- */

static bool
handle_include_paths(rpc_server_st * svr, struct json_object * init_opts)
{
    struct json_object * include_paths_arr = NULL;

    if (json_object_object_get_ex(init_opts, "includePaths", &include_paths_arr)
        && json_object_is_type(include_paths_arr, json_type_array))
    {
        int const count = json_object_array_length(include_paths_arr);
        fprintf(stderr, "[LSP] initialize: received %d include paths\n", count);
        if (count > 0)
        {
            svr->include_paths = calloc((size_t)count, sizeof(*svr->include_paths));
            if (svr->include_paths)
            {
                svr->include_paths_count = count;
                for (int i = 0; i < count; i++)
                {
                    struct json_object * elem = json_object_array_get_idx(include_paths_arr, i);
                    if (json_object_is_type(elem, json_type_string))
                    {
                        svr->include_paths[i] = strdup(json_object_get_string(elem));
                        fprintf(stderr, "[LSP] initialize: include_paths[%d] = \"%s\"\n", i, svr->include_paths[i]);
                    }
                }
            }
        }
    }
    else
    {
        fprintf(stderr, "[LSP] initialize: no includePaths in initializationOptions\n");
    }

    return true;
}

static bool
handle_initialization_options(rpc_server_st * svr, struct json_object * params)
{
    struct json_object * init_opts = NULL;

    if (json_object_object_get_ex(params, "initializationOptions", &init_opts))
    {
        handle_include_paths(svr, init_opts);
    }
    else
    {
        fprintf(stderr, "[LSP] initialize: no initializationOptions\n");
    }

    return true;
}

static bool
handle_initialize(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    handle_initialization_options(svr, params);

    struct json_object * result = json_object_new_object();
    struct json_object * capabilities = json_object_new_object();
    json_object_object_add(capabilities, "textDocumentSync", json_object_new_int(1));
    json_object_object_add(result, "capabilities", capabilities);

    rpc_send_response(svr, id, result);

    return true;
}

static bool
handle_shutdown(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    UNUSED_PARAM(params);

    svr->shutdown_requested = true;
    rpc_send_response(svr, id, NULL);
    return true;
}

static bool
handle_exit(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    UNUSED_PARAM(params);
    UNUSED_PARAM(id);

    transport_close_stdin(svr);

    if (transport_can_exit(svr))
    {
        uloop_end();
    }

    return true;
}

/* --- Document synchronization --- */

static bool
handle_text_document_did_open(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    UNUSED_PARAM(svr);
    UNUSED_PARAM(id);

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
    UNUSED_PARAM(svr);
    UNUSED_PARAM(id);

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
    UNUSED_PARAM(svr);
    UNUSED_PARAM(id);

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

typedef struct complexity_ctx_st
{
    rpc_server_st * svr;
    struct json_object * id;
    tool_run_st tool_run;
} complexity_ctx_st;

static void
complexity_on_complete(tool_run_st * run, void * user_data)
{
    complexity_ctx_st * ctx = user_data;
    rpc_server_st * svr = ctx->svr;
    int complexity_value = -1;
    char func_name_buf[256] = "";
    char const * output = tool_run_output(run);

    if (output != NULL && output[0] != '\0')
    {
        struct json_object * arr = json_tokener_parse(output);
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
                    snprintf(func_name_buf, sizeof(func_name_buf), "%s", json_object_get_string(name_obj));
                }
            }
        }
        json_object_put(arr);
    }

    struct json_object * result = json_object_new_object();

    json_object_object_add(result, "complexity", json_object_new_int(complexity_value));
    if (func_name_buf[0])
    {
        json_object_object_add(result, "function_name", json_object_new_string(func_name_buf));
    }

    rpc_send_response(svr, ctx->id, result);

    if (run->temp_path != NULL)
    {
        unlink(run->temp_path);
        free(run->temp_path);
    }

    free(run->output);
    json_object_put(ctx->id);
    free(ctx);
}

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

    struct json_object * uri_obj = NULL;
    json_object_object_get_ex(text_document_obj, "uri", &uri_obj);
    char const * uri = json_object_get_string(uri_obj);
    char const * function_name = json_object_get_string(func_name_obj);

    fprintf(
        stderr,
        "[LSP] complexity: uri=%s, function=%s\n",
        uri ? uri : "(null)",
        function_name ? function_name : "(null)"
    );

    if (!uri || !function_name)
    {
        return false;
    }

    document_st * doc = documents_lookup(uri);
    if (!doc)
    {
        fprintf(stderr, "[LSP] complexity: document not found: %s\n", uri);
        return false;
    }

    /* Write document text to a temporary file. */
    char tmp_pattern[] = "/tmp/c_tools_XXXXXX.c";
    int tmp_fd = mkstemps(tmp_pattern, 2);
    if (tmp_fd < 0)
    {
        perror("mkstemps");
        return false;
    }

    size_t text_len = strlen(doc->text);
    ssize_t written = write(tmp_fd, doc->text, text_len);
    if (written < 0 || (size_t)written != text_len)
    {
        perror("write");
        close(tmp_fd);
        unlink(tmp_pattern);
        return false;
    }
    close(tmp_fd);

    /* Build argv. */
    int const num_inc_paths = svr->include_paths_count;
    int const argc = 1 + 2 + 1 + (num_inc_paths * 2) + 1 + 1;
    char ** argv = malloc((size_t)argc * sizeof(*argv));

    if (argv == NULL)
    {
        unlink(tmp_pattern);
        return false;
    }

    int arg_idx = 0;

    argv[arg_idx++] = "cyclomatic_complexity";
    argv[arg_idx++] = "-j";
    argv[arg_idx++] = "-f";
    argv[arg_idx++] = (char *)function_name;
    for (int i = 0; i < num_inc_paths; i++)
    {
        if (svr->include_paths[i])
        {
            argv[arg_idx++] = "-I";
            argv[arg_idx++] = svr->include_paths[i];
        }
    }
    argv[arg_idx++] = tmp_pattern;
    argv[arg_idx] = NULL;

    /* Log the command. */
    fprintf(stderr, "[LSP] complexity: command: %s", CYCLOMATIC_COMPLEXITY_PATH);
    for (int i = 1; i < argc; i++)
    {
        fprintf(stderr, " %s", argv[i]);
    }
    fprintf(stderr, "\n");

    /* Allocate context and start the tool. */
    complexity_ctx_st * ctx = calloc(1, sizeof(*ctx));

    if (ctx == NULL)
    {
        free(argv);
        unlink(tmp_pattern);
        return false;
    }

    ctx->svr = svr;
    ctx->id = id != NULL ? json_object_get(id) : NULL;

    tool_run_init(&ctx->tool_run);

    if (!tool_run_start(
            &ctx->tool_run,
            &svr->tool_queue,
            CYCLOMATIC_COMPLEXITY_PATH,
            argv,
            strdup(tmp_pattern),
            complexity_on_complete,
            ctx
        ))
    {
        free(argv);
        unlink(tmp_pattern);
        free(ctx);
        return false;
    }

    free(argv);
    return true;
}

/* --- No-op handlers --- */

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
    rpc_register_method(svr, "initialize", handle_initialize);
    rpc_register_method(svr, "initialized", handle_initialized);
    rpc_register_method(svr, "shutdown", handle_shutdown);
    rpc_register_method(svr, "exit", handle_exit);
    rpc_register_method(svr, "textDocument/didOpen", handle_text_document_did_open);
    rpc_register_method(svr, "textDocument/didChange", handle_text_document_did_change);
    rpc_register_method(svr, "textDocument/didClose", handle_text_document_did_close);
    rpc_register_method(svr, "textDocument/functionComplexity", handle_function_complexity);
}
