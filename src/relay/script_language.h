#ifndef RELAY_SCRIPT_LANGUAGE_H
#define RELAY_SCRIPT_LANGUAGE_H

#include <stdbool.h>
#include <stddef.h>

enum {
    RELAY_SCRIPT_LANGUAGE_MAX_TOKENS = 2048,
    RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS = 16
};

/** Semantic token classes produced by Relay's built-in Lua lexer. */
typedef enum Relay_ScriptTokenKind {
    RELAY_SCRIPT_TOKEN_DEFAULT,
    RELAY_SCRIPT_TOKEN_KEYWORD,
    RELAY_SCRIPT_TOKEN_STRING,
    RELAY_SCRIPT_TOKEN_NUMBER,
    RELAY_SCRIPT_TOKEN_COMMENT,
    RELAY_SCRIPT_TOKEN_FUNCTION,
    RELAY_SCRIPT_TOKEN_TYPE,
    RELAY_SCRIPT_TOKEN_NAMESPACE,
    RELAY_SCRIPT_TOKEN_MEMBER,
    RELAY_SCRIPT_TOKEN_OPERATOR
} Relay_ScriptTokenKind;

/** One source byte range with a stable semantic token class. */
typedef struct Relay_ScriptToken {
    size_t start;
    size_t length;
    Relay_ScriptTokenKind kind;
} Relay_ScriptToken;

/** One context-filtered editor completion item. */
typedef struct Relay_ScriptCompletion {
    const char *label;
    const char *insert_text;
    const char *detail;
} Relay_ScriptCompletion;

/** Caller-owned dynamic symbols available to one completion request. */
typedef struct Relay_ScriptLanguageCatalog {
    const char *const *script_names;
    size_t script_name_count;
} Relay_ScriptLanguageCatalog;

/** Active call signature and zero-based parameter selected at the cursor. */
typedef struct Relay_ScriptSignature {
    const char *label;
    size_t active_parameter;
} Relay_ScriptSignature;

/** Tokenize a complete Blueprint source buffer with multiline lexical state. */
size_t relay_script_language_tokenize(const char *source, size_t source_size,
    Relay_ScriptToken *tokens, size_t token_capacity);

/** Return context-sensitive completions and the prefix range they replace. */
size_t relay_script_language_complete(const char *source, size_t source_size,
    size_t cursor, const Relay_ScriptLanguageCatalog *catalog,
    Relay_ScriptCompletion *completions, size_t completion_capacity,
    size_t *replacement_start);

/** Return signature help for the innermost recognized call at the cursor. */
bool relay_script_language_signature(const char *source, size_t source_size,
    size_t cursor, Relay_ScriptSignature *signature);

#endif
