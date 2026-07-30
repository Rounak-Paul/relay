#include "relay/game.h"
#include "relay/script_language.h"

#include <string.h>

/** Return whether a token of one kind covers the requested source byte. */
static bool relay_test_token_at(const Relay_ScriptToken *tokens,
    size_t token_count, size_t offset, Relay_ScriptTokenKind kind)
{
    size_t index;

    for (index = 0; index < token_count; index++) {
        if (tokens[index].kind == kind && offset >= tokens[index].start &&
            offset < tokens[index].start + tokens[index].length) {
            return true;
        }
    }
    return false;
}

/** Verify semantic highlighting, completion, signature help, and insertion. */
int relay_script_language_test(void)
{
    static const char source[] =
        "-- module\n"
        "local miner = instance(source.iron_miner, { x = 0, y = 0 })\n"
        "function on_process(state, inputs, outputs)\n"
        "  local ore = Type.IRON_ORE\n"
        "  return \"ready\"\n"
        "end\n";
    static const char type_prefix[] = "Type.IR";
    static const char call[] = "input(\"ore\", Type.";
    static const char method_call[] = "string.gsub(\"a,b\", \",\", \"-\")";
    Relay_ScriptToken tokens[64];
    Relay_ScriptCompletion completions[8];
    const char *const script_names[] = {"script_1", "ore_processor"};
    const Relay_ScriptLanguageCatalog catalog = {
        script_names, sizeof(script_names) / sizeof(script_names[0])};
    Relay_ScriptSignature signature;
    Relay_ScriptRuntime runtime = {0};
    Relay_Game game = {0};
    Relay_Blueprint *blueprint;
    size_t replacement_start;
    size_t token_count;
    size_t completion_count;
    size_t index;
    bool found_input = false;

    token_count = relay_script_language_tokenize(source, sizeof(source) - 1,
        tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (!relay_test_token_at(tokens, token_count, 0,
            RELAY_SCRIPT_TOKEN_COMMENT) ||
        !relay_test_token_at(tokens, token_count,
            (size_t)(strstr(source, "function") - source),
            RELAY_SCRIPT_TOKEN_KEYWORD) ||
        !relay_test_token_at(tokens, token_count,
            (size_t)(strstr(source, "on_process") - source),
            RELAY_SCRIPT_TOKEN_FUNCTION) ||
        !relay_test_token_at(tokens, token_count,
            (size_t)(strstr(source, "IRON_ORE") - source),
            RELAY_SCRIPT_TOKEN_TYPE) ||
        !relay_test_token_at(tokens, token_count,
            (size_t)(strstr(source, "source") - source),
            RELAY_SCRIPT_TOKEN_NAMESPACE) ||
        !relay_test_token_at(tokens, token_count,
            (size_t)(strstr(source, "iron_miner") - source),
            RELAY_SCRIPT_TOKEN_MEMBER) ||
        !relay_test_token_at(tokens, token_count,
            (size_t)(strstr(source, "\"ready\"") - source),
            RELAY_SCRIPT_TOKEN_STRING)) {
        return 1;
    }
    completion_count = relay_script_language_complete(type_prefix,
        sizeof(type_prefix) - 1, sizeof(type_prefix) - 1, &catalog,
        completions, sizeof(completions) / sizeof(completions[0]),
        &replacement_start);
    if (completion_count != 1 || replacement_start != 5 ||
        strcmp(completions[0].label, "IRON_ORE") != 0 ||
        !relay_script_language_signature(call, sizeof(call) - 1,
            sizeof(call) - 1, &signature) ||
        strcmp(signature.label, "input(name, type)") != 0 ||
        signature.active_parameter != 1) {
        return 1;
    }
    completion_count = relay_script_language_complete("in", 2, 2, &catalog,
        completions, sizeof(completions) / sizeof(completions[0]),
        &replacement_start);
    for (index = 0; index < completion_count; index++) {
        if (strcmp(completions[index].label, "input") == 0) {
            found_input = true;
        }
    }
    if (!found_input || !relay_script_runtime_init(&runtime,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        !relay_game_init(&game, &runtime) ||
        !relay_game_create_blueprint(&game) ||
        !relay_game_create_blueprint(&game) ||
        !relay_game_open_editor(&game) ||
        !relay_game_editor_enter_insert(&game)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    completion_count = relay_script_language_complete("string.s", 8, 8,
        &catalog, completions,
        sizeof(completions) / sizeof(completions[0]), &replacement_start);
    if (completion_count == 0 || replacement_start != 7 ||
        strcmp(completions[0].label, "sub") != 0 ||
        !relay_script_language_signature(method_call,
            sizeof(method_call) - 2, sizeof(method_call) - 2, &signature) ||
        strcmp(signature.label,
            "string.gsub(value, pattern, replacement [, count])") != 0 ||
        signature.active_parameter != 2) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    completion_count = relay_script_language_complete("source.co", 9, 9,
        &catalog, completions,
        sizeof(completions) / sizeof(completions[0]), &replacement_start);
    if (completion_count != 2 || replacement_start != 7 ||
        strcmp(completions[0].label, "coal_miner") != 0 ||
        strcmp(completions[1].label, "copper_miner") != 0) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    completion_count = relay_script_language_complete("script.ore", 10, 10,
        &catalog, completions,
        sizeof(completions) / sizeof(completions[0]), &replacement_start);
    if (completion_count != 1 || replacement_start != 7 ||
        strcmp(completions[0].label, "ore_processor") != 0) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    blueprint = relay_game_editing_blueprint(&game);
    for (index = 0; index < sizeof(type_prefix) - 1; index++) {
        if (!relay_game_editor_insert(&game, type_prefix[index])) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&runtime);
            return 1;
        }
    }
    if (!relay_game_editor_completion_accept(&game) ||
        blueprint == NULL ||
        strncmp(blueprint->source, "Type.IRON_ORE", 13) != 0) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    (void)memcpy(blueprint->source, "script.s", sizeof("script.s"));
    blueprint->source_size = sizeof("script.s") - 1;
    blueprint->cursor = blueprint->source_size;
    blueprint->completion_suppressed = false;
    completion_count = relay_game_editor_completions(&game, completions,
        sizeof(completions) / sizeof(completions[0]), &replacement_start);
    if (completion_count != 1 || replacement_start != 7 ||
        strcmp(completions[0].label, "script_1") != 0) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    relay_game_shutdown(&game);
    relay_script_runtime_shutdown(&runtime);
    return 0;
}
