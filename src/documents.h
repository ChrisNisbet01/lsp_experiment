#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct document_st
{
    char * uri;
    char * text;
} document_st;

void documents_init(void);
void documents_cleanup(void);

document_st * documents_lookup(char const * uri);

void documents_add(document_st * doc);
void documents_remove(char const * uri);
void documents_update(char const * uri, char const * text);
