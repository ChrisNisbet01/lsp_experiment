#include "documents.h"

#include "utils.h"

#include <stdlib.h>
#include <string.h>

static document_st * docs = NULL;
static size_t doc_count = 0;
static size_t doc_capacity = 0;

void
documents_init(void)
{
    docs = NULL;
    doc_count = 0;
    doc_capacity = 0;
}

void
documents_cleanup(void)
{
    for (size_t i = 0; i < doc_count; i++)
    {
        free(docs[i].uri);
        free(docs[i].text);
    }
    free(docs);
    docs = NULL;
    doc_count = 0;
    doc_capacity = 0;
}

document_st *
documents_lookup(char const * uri)
{
    for (size_t i = 0; i < doc_count; i++)
    {
        if (strcmp(docs[i].uri, uri) == 0)
        {
            return &docs[i];
        }
    }
    return NULL;
}

static void
documents_ensure_capacity(void)
{
    if (doc_count == doc_capacity)
    {
        doc_capacity = doc_capacity == 0 ? 4 : doc_capacity * 2;
        docs = realloc(docs, doc_capacity * sizeof(*docs));
    }
}

void
documents_add(document_st * doc)
{
    documents_ensure_capacity();
    docs[doc_count].uri = strdup(doc->uri);
    docs[doc_count].text = strdup(doc->text);
    doc_count++;
}

void
documents_remove(char const * uri)
{
    for (size_t i = 0; i < doc_count; i++)
    {
        if (strcmp(docs[i].uri, uri) == 0)
        {
            free(docs[i].uri);
            free(docs[i].text);
            docs[i] = docs[doc_count - 1];
            doc_count--;
            return;
        }
    }
}

void
documents_update(char const * uri, char const * text)
{
    document_st * doc = documents_lookup(uri);
    if (doc != NULL)
    {
        free(doc->text);
        doc->text = strdup(text);
    }
    else
    {
        document_st new_doc;
        new_doc.uri = (char *)uri;
        new_doc.text = (char *)text;
        documents_add(&new_doc);
    }
}
