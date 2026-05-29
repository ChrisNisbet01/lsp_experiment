#include "rpc.h"

#include "server.h"
#include "transport.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
rpc_register_method(rpc_server_st * svr, char const * name, rpc_handler_fn handler)
{
    if (svr->registry.count == svr->registry.capacity)
    {
        svr->registry.capacity = svr->registry.capacity == 0 ? 8 : svr->registry.capacity * 2;
        svr->registry.methods = realloc(svr->registry.methods, svr->registry.capacity * sizeof(*svr->registry.methods));
    }

    size_t const idx = svr->registry.count;
    svr->registry.methods[idx].name = strdup(name);
    svr->registry.methods[idx].handler = handler;
    svr->registry.count++;
}

static void
rpc_send_inner(rpc_server_st * svr, struct json_object * msg)
{
    char const * json_str = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
    size_t const json_len = strlen(json_str);

    char header[64];
    int hdr_len = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", json_len);

    size_t total_len = (size_t)hdr_len + json_len;
    char * framed = malloc(total_len);

    if (framed == NULL)
    {
        return;
    }
    memcpy(framed, header, (size_t)hdr_len);
    memcpy(framed + (size_t)hdr_len, json_str, json_len);

    transport_send(svr, framed, total_len);
    free(framed);
}

void
rpc_send_response(rpc_server_st * svr, struct json_object * id, struct json_object * result)
{
    struct json_object * msg = json_object_new_object();
    json_object_object_add(msg, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(msg, "result", result);
    if (id)
    {
        json_object_object_add(msg, "id", json_object_get(id));
    }
    else
    {
        json_object_object_add(msg, "id", NULL);
    }

    rpc_send_inner(svr, msg);
    json_object_put(msg);
}

void
rpc_send_error(rpc_server_st * svr, struct json_object * id, int code, char const * message)
{
    struct json_object * msg = json_object_new_object();
    json_object_object_add(msg, "jsonrpc", json_object_new_string("2.0"));

    struct json_object * error = json_object_new_object();
    json_object_object_add(error, "code", json_object_new_int(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(msg, "error", error);

    if (id)
    {
        json_object_object_add(msg, "id", json_object_get(id));
    }
    else
    {
        json_object_object_add(msg, "id", NULL);
    }

    rpc_send_inner(svr, msg);
    json_object_put(msg);
}

void
rpc_dispatch(rpc_server_st * svr, struct json_object * msg)
{
    struct json_object * id = NULL;
    struct json_object * method_obj = NULL;
    struct json_object * params = NULL;
    struct json_object * version = NULL;

    if (!json_object_is_type(msg, json_type_object))
    {
        fprintf(stderr, "[RPC] Error: message is not a JSON object\n");
        return;
    }

    if (!json_object_object_get_ex(msg, "jsonrpc", &version) || strcmp(json_object_get_string(version), "2.0") != 0)
    {
        fprintf(stderr, "[RPC] Error: invalid JSON-RPC version\n");
        return;
    }

    json_object_object_get_ex(msg, "id", &id);

    if (!json_object_object_get_ex(msg, "method", &method_obj) || !json_object_is_type(method_obj, json_type_string))
    {
        fprintf(stderr, "[RPC] Error: invalid method\n");
        if (id)
        {
            rpc_send_error(svr, id, -32600, "Invalid Request");
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
            fprintf(stderr, "[RPC] Error: handler failed for '%s'\n", method_name);
            rpc_send_error(svr, id, -32600, "Invalid Request");
        }
    }
    else if (id)
    {
        fprintf(stderr, "[RPC] Error: method '%s' not found\n", method_name);
        rpc_send_error(svr, id, -32601, "Method not found");
    }
    else
    {
        fprintf(stderr, "[RPC] Unhandled notification: %s\n", method_name);
    }
}

void
rpc_on_transport_msg(char const * body, size_t len, void * user_data)
{
    (void)len;
    rpc_server_st * svr = (rpc_server_st *)user_data;

    struct json_object * msg = json_tokener_parse(body);
    if (msg)
    {
        rpc_dispatch(svr, msg);
        json_object_put(msg);
    }
    else
    {
        fprintf(stderr, "[RPC] Error: failed to parse JSON body\n");
    }
}

void
rpc_cleanup_registry(rpc_server_st * svr)
{
    for (size_t i = 0; i < svr->registry.count; i++)
    {
        free(svr->registry.methods[i].name);
    }
    free(svr->registry.methods);
    svr->registry.count = 0;
    svr->registry.capacity = 0;
}
