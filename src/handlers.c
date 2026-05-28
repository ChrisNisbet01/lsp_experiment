#include "handlers.h"

#include "documents.h"
#include "server.h"
#include "utils.h"

#include <ctype.h>
#include <libubox/uloop.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    json_object_object_add(capabilities, "hoverProvider", json_object_new_boolean(true));
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

    /* Stop reading stdin so no more callbacks fire during teardown. */
    uloop_fd_delete(&svr->stdin_fd);
    svr->eof_reached = true;

    if (list_empty(&svr->write_queue))
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

/* --- Completion feature --- */

static bool
handle_text_document_completion(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)svr;

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

    char const * uri = json_object_get_string(uri_obj);
    document_st * doc = documents_lookup(uri);
    if (doc == NULL)
    {
        return false;
    }

    /* Collect unique words. */
    size_t word_cap = 128;
    size_t word_count = 0;
    char ** words = malloc(word_cap * sizeof(*words));
    bool in_word = false;
    char word_buf[256];
    size_t word_pos = 0;

    for (char const * p = doc->text; *p != '\0'; p++)
    {
        if (isspace((unsigned char)*p))
        {
            if (in_word)
            {
                word_buf[word_pos] = '\0';
                if (word_pos > 0)
                {
                    bool seen = false;
                    for (size_t i = 0; i < word_count; i++)
                    {
                        if (strcmp(words[i], word_buf) == 0)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        if (word_count == word_cap)
                        {
                            word_cap *= 2;
                            words = realloc(words, word_cap * sizeof(*words));
                        }
                        words[word_count] = strdup(word_buf);
                        word_count++;
                    }
                }
                in_word = false;
                word_pos = 0;
            }
        }
        else if (!in_word)
        {
            in_word = true;
            word_buf[0] = *p;
            word_pos = 1;
        }
        else
        {
            if (word_pos < sizeof(word_buf) - 1)
            {
                word_buf[word_pos++] = *p;
            }
        }
    }

    /* Handle last word. */
    if (in_word && word_pos > 0)
    {
        word_buf[word_pos] = '\0';
        bool seen = false;
        for (size_t i = 0; i < word_count; i++)
        {
            if (strcmp(words[i], word_buf) == 0)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            if (word_count == word_cap)
            {
                word_cap *= 2;
                words = realloc(words, word_cap * sizeof(*words));
            }
            words[word_count] = strdup(word_buf);
            word_count++;
        }
    }

    /* Build completion items array. */
    struct json_object * items = json_object_new_array();
    for (size_t i = 0; i < word_count; i++)
    {
        struct json_object * item = json_object_new_object();
        json_object_object_add(item, "label", json_object_new_string(words[i]));
        json_object_object_add(item, "kind", json_object_new_int(1)); /* Text completion kind */
        json_object_object_add(item, "detail", json_object_new_string("word"));
        json_object_array_add(items, item);
        free(words[i]);
    }
    free(words);

    struct json_object * result = json_object_new_object();
    json_object_object_add(result, "isIncomplete", json_object_new_boolean(false));
    json_object_object_add(result, "items", items);

    queue_success_response(svr, id, result);

    return true;
}

/* --- Hover feature --- */

static size_t
count_words(char const * text)
{
    size_t count = 0;
    bool in_word = false;

    for (char const * p = text; *p != '\0'; p++)
    {
        if (isspace((unsigned char)*p))
        {
            in_word = false;
        }
        else if (!in_word)
        {
            in_word = true;
            count++;
        }
    }

    return count;
}

static bool
handle_text_document_hover(rpc_server_st * svr, struct json_object * params, struct json_object * id)
{
    (void)svr;

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

    char const * uri = json_object_get_string(uri_obj);
    document_st * doc = documents_lookup(uri);
    if (doc == NULL)
    {
        return false;
    }

    size_t words = count_words(doc->text);
    size_t chars = strlen(doc->text);

    char value[256];
    snprintf(value, sizeof(value), "**Toy LSP — Document Stats**\n- Words: %zu\n- Characters: %zu", words, chars);

    struct json_object * markup = json_object_new_object();
    json_object_object_add(markup, "kind", json_object_new_string("markdown"));
    json_object_object_add(markup, "value", json_object_new_string(value));

    struct json_object * result = json_object_new_object();
    json_object_object_add(result, "contents", markup);

    queue_success_response(svr, id, result);

    return true;
}

void
rpc_server_register_handlers(rpc_server_st * svr)
{
    rpc_server_register_method(svr, "initialize", handle_initialize);
    rpc_server_register_method(svr, "shutdown", handle_shutdown);
    rpc_server_register_method(svr, "exit", handle_exit);
    rpc_server_register_method(svr, "textDocument/didOpen", handle_text_document_did_open);
    rpc_server_register_method(svr, "textDocument/didChange", handle_text_document_did_change);
    rpc_server_register_method(svr, "textDocument/didClose", handle_text_document_did_close);
    rpc_server_register_method(svr, "textDocument/completion", handle_text_document_completion);
    rpc_server_register_method(svr, "textDocument/hover", handle_text_document_hover);
}
