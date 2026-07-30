#include "relay/script_language.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

/** Completion and signature metadata for one built-in callable. */
typedef struct Relay_ScriptCallable {
    const char *name;
    const char *signature;
    const char *detail;
} Relay_ScriptCallable;

static const char *const relay_script_keywords[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while"
};

static const Relay_ScriptCallable relay_script_callables[] = {
    {"input", "input(name, type)", "Declare a typed module input"},
    {"output", "output(name, type)", "Declare a typed module output"},
    {"instance", "instance(definition, layout)", "Declare a component instance"},
    {"connect", "connect(source, destination)", "Connect typed architecture ports"},
    {"on_process", "on_process(state, inputs, outputs)",
        "Blueprint activation observer"},
    {"assert", "assert(value [, message])", "Assert a value"},
    {"error", "error(message [, level])", "Raise a script error"},
    {"ipairs", "ipairs(table)", "Iterate an array in order"},
    {"pcall", "pcall(function, ...)", "Protected function call"},
    {"select", "select(index, ...)", "Select variadic values"},
    {"tostring", "tostring(value)", "Convert a value to text"},
    {"type", "type(value)", "Return a Lua value type"},
    {"xpcall", "xpcall(function, handler, ...)", "Protected call with handler"},
    {"string.byte", "string.byte(value [, first [, last]])", "Read string bytes"},
    {"string.char", "string.char(...)", "Create a string from bytes"},
    {"string.find", "string.find(value, pattern [, init [, plain]])", "Find a pattern"},
    {"string.gmatch", "string.gmatch(value, pattern)", "Iterate pattern matches"},
    {"string.gsub", "string.gsub(value, pattern, replacement [, count])", "Replace matches"},
    {"string.len", "string.len(value)", "Return string length"},
    {"string.lower", "string.lower(value)", "Convert to lowercase"},
    {"string.match", "string.match(value, pattern [, init])", "Match a pattern"},
    {"string.rep", "string.rep(value, count [, separator])", "Repeat a string"},
    {"string.reverse", "string.reverse(value)", "Reverse a string"},
    {"string.sub", "string.sub(value, first [, last])", "Slice a string"},
    {"string.upper", "string.upper(value)", "Convert to uppercase"},
    {"table.concat", "table.concat(list [, separator [, first [, last]]])", "Join array values"},
    {"table.insert", "table.insert(list, [position,] value)", "Insert an array value"},
    {"table.move", "table.move(source, first, last, target [, destination])", "Move array values"},
    {"table.pack", "table.pack(...)", "Pack values into an array"},
    {"table.remove", "table.remove(list [, position])", "Remove an array value"},
    {"table.sort", "table.sort(list [, compare])", "Sort an array"},
    {"table.unpack", "table.unpack(list [, first [, last]])", "Unpack array values"},
    {"utf8.char", "utf8.char(...)", "Encode code points"},
    {"utf8.codes", "utf8.codes(value)", "Iterate UTF-8 code points"},
    {"utf8.codepoint", "utf8.codepoint(value [, first [, last]])", "Read code points"},
    {"utf8.len", "utf8.len(value [, first [, last]])", "Count UTF-8 characters"},
    {"utf8.offset", "utf8.offset(value, count [, position])", "Locate a UTF-8 character"}
};

static const char *const relay_script_type_members[] = {
    "TRIGGER", "COAL", "IRON_ORE", "COPPER_ORE", "STONE", "BOOLEAN", "INTEGER"
};

static const char *const relay_script_namespaces[] = {
    "source", "control", "script"
};

static const char *const relay_script_source_members[] = {
    "coal_miner", "iron_miner", "copper_miner", "stone_miner"
};

static const char *const relay_script_control_members[] = {
    "timer"
};

static const char *const relay_script_globals[] = {
    "Type", "source", "control", "script", "inputs", "outputs", "state",
    "string", "table", "utf8"
};

/** Return whether one byte can occur in a Lua identifier. */
static bool relay_script_identifier_byte(char value)
{
    return isalnum((unsigned char)value) || value == '_';
}

/** Return whether an identifier exactly matches one list entry. */
static bool relay_script_word_in(const char *word, size_t length,
    const char *const *entries, size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        if (strlen(entries[index]) == length &&
            memcmp(word, entries[index], length) == 0) {
            return true;
        }
    }
    return false;
}

/** Return whether an identifier names one built-in callable. */
static bool relay_script_callable_find(const char *word, size_t length,
    size_t *callable_index)
{
    size_t index;

    for (index = 0; index < sizeof(relay_script_callables) /
            sizeof(relay_script_callables[0]); index++) {
        if (strlen(relay_script_callables[index].name) == length &&
            memcmp(word, relay_script_callables[index].name, length) == 0) {
            if (callable_index != NULL) {
                *callable_index = index;
            }
            return true;
        }
    }
    return false;
}

/** Append one non-empty token while respecting caller-owned capacity. */
static void relay_script_token_add(Relay_ScriptToken *tokens, size_t capacity,
    size_t *count, size_t start, size_t end, Relay_ScriptTokenKind kind)
{
    if (end <= start) {
        return;
    }
    if (*count < capacity) {
        tokens[*count] = (Relay_ScriptToken){start, end - start, kind};
    }
    (*count)++;
}

/** Return the token class covering one byte from a sorted token stream. */
static Relay_ScriptTokenKind relay_script_token_kind_at(
    const Relay_ScriptToken *tokens, size_t token_count, size_t offset)
{
    size_t low = 0;
    size_t high = token_count;

    while (low < high) {
        const size_t middle = low + (high - low) / 2;

        if (offset < tokens[middle].start) {
            high = middle;
        } else if (offset >= tokens[middle].start + tokens[middle].length) {
            low = middle + 1;
        } else {
            return tokens[middle].kind;
        }
    }
    return RELAY_SCRIPT_TOKEN_DEFAULT;
}

size_t relay_script_language_tokenize(const char *source, size_t source_size,
    Relay_ScriptToken *tokens, size_t token_capacity)
{
    size_t count = 0;
    size_t index = 0;

    if (source == NULL || (tokens == NULL && token_capacity > 0)) {
        return 0;
    }
    while (index < source_size) {
        const size_t start = index;
        Relay_ScriptTokenKind kind = RELAY_SCRIPT_TOKEN_DEFAULT;

        if (isspace((unsigned char)source[index])) {
            index++;
            continue;
        }
        if (source[index] == '-' && index + 1 < source_size &&
            source[index + 1] == '-') {
            index += 2;
            if (index + 1 < source_size && source[index] == '[' &&
                source[index + 1] == '[') {
                index += 2;
                while (index + 1 < source_size &&
                    !(source[index] == ']' && source[index + 1] == ']')) {
                    index++;
                }
                index = index + 1 < source_size ? index + 2 : source_size;
            } else {
                while (index < source_size && source[index] != '\n') index++;
            }
            kind = RELAY_SCRIPT_TOKEN_COMMENT;
        } else if (source[index] == '\'' || source[index] == '"') {
            const char quote = source[index++];

            while (index < source_size) {
                if (source[index] == '\\' && index + 1 < source_size) {
                    index += 2;
                } else if (source[index++] == quote) {
                    break;
                }
            }
            kind = RELAY_SCRIPT_TOKEN_STRING;
        } else if (source[index] == '[' && index + 1 < source_size &&
            source[index + 1] == '[') {
            index += 2;
            while (index + 1 < source_size &&
                !(source[index] == ']' && source[index + 1] == ']')) index++;
            index = index + 1 < source_size ? index + 2 : source_size;
            kind = RELAY_SCRIPT_TOKEN_STRING;
        } else if (isdigit((unsigned char)source[index])) {
            index++;
            while (index < source_size &&
                (isalnum((unsigned char)source[index]) ||
                    source[index] == '_')) index++;
            kind = RELAY_SCRIPT_TOKEN_NUMBER;
        } else if (isalpha((unsigned char)source[index]) ||
            source[index] == '_') {
            bool type_member = false;

            index++;
            while (index < source_size &&
                relay_script_identifier_byte(source[index])) index++;
            if (start >= 5 && source[start - 1] == '.' &&
                memcmp(&source[start - 5], "Type.", 5) == 0) {
                type_member = true;
            }
            if (type_member || (index - start == 4 &&
                    memcmp(&source[start], "Type", 4) == 0)) {
                kind = RELAY_SCRIPT_TOKEN_TYPE;
            } else if (relay_script_word_in(&source[start], index - start,
                    relay_script_namespaces,
                    sizeof(relay_script_namespaces) /
                        sizeof(relay_script_namespaces[0]))) {
                kind = RELAY_SCRIPT_TOKEN_NAMESPACE;
            } else if (relay_script_word_in(&source[start], index - start,
                    relay_script_keywords, sizeof(relay_script_keywords) /
                        sizeof(relay_script_keywords[0]))) {
                kind = RELAY_SCRIPT_TOKEN_KEYWORD;
            } else if (relay_script_callable_find(&source[start],
                    index - start, NULL)) {
                kind = RELAY_SCRIPT_TOKEN_FUNCTION;
            } else if (start > 0 && source[start - 1] == '.') {
                size_t qualifier_start = start - 1;

                while (qualifier_start > 0 &&
                    relay_script_identifier_byte(
                        source[qualifier_start - 1])) qualifier_start--;
                kind = relay_script_callable_find(&source[qualifier_start],
                    index - qualifier_start, NULL) ?
                    RELAY_SCRIPT_TOKEN_FUNCTION : RELAY_SCRIPT_TOKEN_MEMBER;
            }
        } else {
            index++;
            if (index < source_size &&
                ((source[start] == '=' && source[index] == '=') ||
                 (source[start] == '~' && source[index] == '=') ||
                 (source[start] == '<' && source[index] == '=') ||
                 (source[start] == '>' && source[index] == '=') ||
                 (source[start] == '/' && source[index] == '/'))) {
                index++;
            }
            kind = RELAY_SCRIPT_TOKEN_OPERATOR;
        }
        relay_script_token_add(tokens, token_capacity, &count, start, index,
            kind);
    }
    return count < token_capacity ? count : token_capacity;
}

/** Append matching completion entries from one string list. */
static void relay_script_complete_words(const char *const *words, size_t count,
    const char *prefix, size_t prefix_size, const char *detail,
    Relay_ScriptCompletion *items, size_t capacity, size_t *item_count)
{
    size_t index;

    for (index = 0; index < count && *item_count < capacity; index++) {
        if (strncmp(words[index], prefix, prefix_size) == 0) {
            items[(*item_count)++] = (Relay_ScriptCompletion){
                words[index], words[index], detail};
        }
    }
}

size_t relay_script_language_complete(const char *source, size_t source_size,
    size_t cursor, const Relay_ScriptLanguageCatalog *catalog,
    Relay_ScriptCompletion *completions, size_t completion_capacity,
    size_t *replacement_start)
{
    size_t start;
    size_t count = 0;
    size_t index;
    const char *prefix;
    size_t prefix_size;

    if (source == NULL || cursor > source_size || completions == NULL ||
        replacement_start == NULL) {
        return 0;
    }
    start = cursor;
    while (start > 0 && relay_script_identifier_byte(source[start - 1])) start--;
    prefix = &source[start];
    prefix_size = cursor - start;
    *replacement_start = start;
    if (start >= 5 && source[start - 1] == '.' &&
        memcmp(&source[start - 5], "Type.", 5) == 0) {
        relay_script_complete_words(relay_script_type_members,
            sizeof(relay_script_type_members) /
                sizeof(relay_script_type_members[0]),
            prefix, prefix_size, "Relay port type", completions,
            completion_capacity, &count);
        return count;
    }
    if (start > 0 && source[start - 1] == '.') {
        size_t qualifier_start = start - 1;
        size_t qualifier_size;

        while (qualifier_start > 0 &&
            relay_script_identifier_byte(source[qualifier_start - 1])) {
            qualifier_start--;
        }
        qualifier_size = start - 1 - qualifier_start;
        if (qualifier_size == 6 &&
            memcmp(&source[qualifier_start], "source", 6) == 0) {
            relay_script_complete_words(relay_script_source_members,
                sizeof(relay_script_source_members) /
                    sizeof(relay_script_source_members[0]),
                prefix, prefix_size, "Built-in source node", completions,
                completion_capacity, &count);
            return count;
        }
        if (qualifier_size == 7 &&
            memcmp(&source[qualifier_start], "control", 7) == 0) {
            relay_script_complete_words(relay_script_control_members,
                sizeof(relay_script_control_members) /
                    sizeof(relay_script_control_members[0]),
                prefix, prefix_size, "Built-in control node", completions,
                completion_capacity, &count);
            return count;
        }
        if (qualifier_size == 6 &&
            memcmp(&source[qualifier_start], "script", 6) == 0) {
            if (catalog != NULL && catalog->script_names != NULL) {
                relay_script_complete_words(catalog->script_names,
                    catalog->script_name_count, prefix, prefix_size,
                    "Reusable script", completions, completion_capacity,
                    &count);
            }
            return count;
        }
        for (index = 0; index < sizeof(relay_script_callables) /
                sizeof(relay_script_callables[0]) &&
                count < completion_capacity; index++) {
            const char *dot = strchr(relay_script_callables[index].name, '.');
            const char *member;

            if (dot == NULL ||
                (size_t)(dot - relay_script_callables[index].name) !=
                    qualifier_size ||
                memcmp(relay_script_callables[index].name,
                    &source[qualifier_start], qualifier_size) != 0) {
                continue;
            }
            member = dot + 1;
            if (strncmp(member, prefix, prefix_size) == 0) {
                completions[count++] = (Relay_ScriptCompletion){
                    member, member, relay_script_callables[index].detail};
            }
        }
        return count;
    }
    if (prefix_size == 0) {
        return 0;
    }
    for (index = 0; index < sizeof(relay_script_callables) /
            sizeof(relay_script_callables[0]) && count < completion_capacity;
            index++) {
        if (strchr(relay_script_callables[index].name, '.') == NULL &&
            strncmp(relay_script_callables[index].name, prefix,
                prefix_size) == 0) {
            completions[count++] = (Relay_ScriptCompletion){
                relay_script_callables[index].name,
                relay_script_callables[index].name,
                relay_script_callables[index].detail};
        }
    }
    relay_script_complete_words(relay_script_keywords,
        sizeof(relay_script_keywords) / sizeof(relay_script_keywords[0]),
        prefix, prefix_size, "Lua keyword", completions, completion_capacity,
        &count);
    relay_script_complete_words(relay_script_globals,
        sizeof(relay_script_globals) / sizeof(relay_script_globals[0]),
        prefix, prefix_size, "Relay/Lua global", completions,
        completion_capacity, &count);
    return count;
}

bool relay_script_language_signature(const char *source, size_t source_size,
    size_t cursor, Relay_ScriptSignature *signature)
{
    Relay_ScriptToken tokens[RELAY_SCRIPT_LANGUAGE_MAX_TOKENS];
    const size_t token_count = relay_script_language_tokenize(source,
        source_size, tokens, RELAY_SCRIPT_LANGUAGE_MAX_TOKENS);
    size_t index;
    size_t open = SIZE_MAX;
    size_t depth = 0;
    size_t argument = 0;
    size_t name_end;
    size_t name_start;
    size_t callable_index;

    if (source == NULL || signature == NULL || cursor > source_size) {
        return false;
    }
    for (index = cursor; index > 0; index--) {
        const char value = source[index - 1];
        const Relay_ScriptTokenKind kind = relay_script_token_kind_at(tokens,
            token_count, index - 1);

        if (kind == RELAY_SCRIPT_TOKEN_STRING ||
            kind == RELAY_SCRIPT_TOKEN_COMMENT) {
            continue;
        }
        if (value == ')' || value == ']' || value == '}') {
            depth++;
        } else if (value == '(' || value == '[' || value == '{') {
            if (depth == 0) {
                if (value != '(') {
                    return false;
                }
                open = index - 1;
                break;
            }
            depth--;
        } else if (value == ',' && depth == 0) {
            argument++;
        }
    }
    if (open == SIZE_MAX) {
        return false;
    }
    name_end = open;
    while (name_end > 0 && isspace((unsigned char)source[name_end - 1])) {
        name_end--;
    }
    name_start = name_end;
    while (name_start > 0 &&
        (relay_script_identifier_byte(source[name_start - 1]) ||
            source[name_start - 1] == '.')) name_start--;
    if (!relay_script_callable_find(&source[name_start],
            name_end - name_start, &callable_index)) {
        return false;
    }
    signature->label = relay_script_callables[callable_index].signature;
    signature->active_parameter = argument;
    return true;
}
