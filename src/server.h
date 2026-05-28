#pragma once

#include <json-c/json.h>
#include <libubox/list.h>
#include <libubox/runqueue.h>
#include <libubox/uloop.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct rpc_server_st rpc_server_st;

typedef bool (*rpc_handler_fn)(rpc_server_st * svr, struct json_object * params, struct json_object * id);

typedef struct rpc_method_st
{
    char * name;
    rpc_handler_fn handler;
} rpc_method_st;

typedef struct rpc_method_registry_st
{
    rpc_method_st * methods;
    size_t count;
    size_t capacity;
} rpc_method_registry_st;

typedef struct rpc_server_st
{
    int in_fd;
    int out_fd;

    struct uloop_fd stdin_fd;
    struct uloop_fd out_uloop_fd;
    struct list_head write_queue;
    struct runqueue tool_queue;

    /* Parsing state for Content-Length framed protocol. */
    char * buf;
    size_t buf_len;
    size_t buf_cap;
    int content_length;
    bool in_header;

    rpc_method_registry_st registry;

    bool shutdown_requested;
    bool eof_reached;
    int exit_code;
} rpc_server_st;

void rpc_server_register_method(rpc_server_st * svr, char const * name, rpc_handler_fn handler);

void rpc_server_queue_response(rpc_server_st * svr, struct json_object * res);

void run_server(rpc_server_st * svr, int const in_fd, int const out_fd);
