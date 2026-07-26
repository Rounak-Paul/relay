#include <stdio.h>
#include <stdlib.h>

/** Embed one binary file as a C header containing an immutable byte array. */
int main(int argument_count, char **arguments)
{
    FILE *input = NULL;
    FILE *output = NULL;
    unsigned char buffer[4096];
    size_t read_count;
    size_t total = 0;
    size_t column = 0;

    if (argument_count != 3 || (input = fopen(arguments[1], "rb")) == NULL ||
        (output = fopen(arguments[2], "w")) == NULL) {
        if (input != NULL) {
            (void)fclose(input);
        }
        return 1;
    }
    (void)fputs("#ifndef RELAY_DEPARTURE_MONO_FONT_H\n", output);
    (void)fputs("#define RELAY_DEPARTURE_MONO_FONT_H\n\n", output);
    (void)fputs("#include <stddef.h>\n\n", output);
    (void)fputs("static const unsigned char relay_departure_mono_font_data[] = {\n",
        output);
    while ((read_count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        size_t index;

        for (index = 0; index < read_count; index++) {
            if (column == 0) {
                (void)fputs("    ", output);
            }
            (void)fprintf(output, "0x%02X, ", buffer[index]);
            column = (column + 1) % 12;
            if (column == 0) {
                (void)fputc('\n', output);
            }
            total++;
        }
    }
    if (ferror(input) || ferror(output)) {
        (void)fclose(input);
        (void)fclose(output);
        return 1;
    }
    if (column != 0) {
        (void)fputc('\n', output);
    }
    (void)fputs("};\n", output);
    (void)fprintf(output,
        "static const size_t relay_departure_mono_font_size = %zu;\n\n", total);
    (void)fputs("#endif\n", output);
    return fclose(input) == 0 && fclose(output) == 0 ? 0 : 1;
}
