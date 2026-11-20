#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 128

static void *json_realloc_array(void *items, size_t count, size_t item_size);
static char *json_strdup_range(const char *start, size_t length);

typedef struct {
    const char *text;
    const char *cursor;
    char error[JSON_ERROR_SIZE];
    size_t depth;
} JsonParser;

static void json_set_error(JsonParser *parser, const char *message) {
    if (parser->error[0])
        return;
    (void)snprintf(parser->error, sizeof(parser->error), "%s", message);
}

JsonValue *json_value_new(JsonValueType type) {
    JsonValue *value = (JsonValue *)calloc(1, sizeof(*value));
    if (value)
        value->type = type;
    return value;
}

JsonValue *json_string_new(const char *text) {
    JsonValue *value = json_value_new(JSON_VALUE_STRING);
    if (!value)
        return NULL;
    value->string = json_strdup_range(text ? text : "", strlen(text ? text : ""));
    if (!value->string) {
        json_value_free(value);
        return NULL;
    }
    return value;
}

JsonValue *json_number_new(const char *literal) {
    JsonValue *value = json_value_new(JSON_VALUE_NUMBER);
    if (!value)
        return NULL;
    value->string = json_strdup_range(literal ? literal : "0",
                                      strlen(literal ? literal : "0"));
    if (!value->string) {
        json_value_free(value);
        return NULL;
    }
    return value;
}

bool json_object_add(JsonValue *object, const char *key, JsonValue *value) {
    if (!object || object->type != JSON_VALUE_OBJECT || !key || !value)
        return false;
    JsonMember *resized = (JsonMember *)json_realloc_array(
        object->members, object->member_count + 1, sizeof(*object->members));
    if (!resized)
        return false;
    object->members = resized;
    object->members[object->member_count].key = json_strdup_range(key, strlen(key));
    if (!object->members[object->member_count].key)
        return false;
    object->members[object->member_count].value = value;
    object->member_count++;
    return true;
}

static void *json_realloc_array(void *items, size_t count, size_t item_size) {
    if (count > (size_t)-1 / item_size)
        return NULL;
    return realloc(items, count * item_size);
}

static char *json_strdup_range(const char *start, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

/* Encodes one Unicode code point as UTF-8. Returns bytes written (0 on
   overflow of the 4-byte buffer, which cannot happen for valid input). */
static size_t json_encode_utf8(char *out, unsigned long code_point) {
    if (code_point < 0x80) {
        out[0] = (char)code_point;
        return 1;
    }
    if (code_point < 0x800) {
        out[0] = (char)(0xC0 | (code_point >> 6));
        out[1] = (char)(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000) {
        out[0] = (char)(0xE0 | (code_point >> 12));
        out[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code_point & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (code_point >> 18));
    out[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    out[3] = (char)(0x80 | (code_point & 0x3F));
    return 4;
}

static int json_hex_digit(char digit) {
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

static bool json_parse_hex4(JsonParser *parser, unsigned long *out) {
    unsigned long value = 0;
    for (int index = 0; index < 4; index++) {
        char digit = *parser->cursor;
        int nibble = json_hex_digit(digit);
        if (nibble < 0) {
            json_set_error(parser, "Invalid unicode escape in JSON string.");
            return false;
        }
        value = value * 16 + (unsigned long)nibble;
        parser->cursor++;
    }
    *out = value;
    return true;
}

static JsonValue *json_parse_string(JsonParser *parser) {
    /* cursor is just past the opening quote. */
    size_t capacity = 32;
    size_t length = 0;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        json_set_error(parser, "Out of memory while parsing JSON string.");
        return NULL;
    }
    while (*parser->cursor && *parser->cursor != '"') {
        unsigned char byte = (unsigned char)*parser->cursor;
        if (byte < 0x20) {
            free(buffer);
            json_set_error(parser, "Unescaped control character in JSON string.");
            return NULL;
        }
        if (byte != '\\') {
            if (length + 1 > capacity) {
                capacity *= 2;
                char *resized = (char *)realloc(buffer, capacity);
                if (!resized) {
                    free(buffer);
                    json_set_error(parser, "Out of memory while parsing JSON string.");
                    return NULL;
                }
                buffer = resized;
            }
            buffer[length++] = (char)byte;
            parser->cursor++;
            continue;
        }
        parser->cursor++;
        char escape = *parser->cursor;
        if (!escape) {
            free(buffer);
            json_set_error(parser, "Truncated escape in JSON string.");
            return NULL;
        }
        parser->cursor++;
        char simple = 0;
        switch (escape) {
        case '"': simple = '"'; break;
        case '\\': simple = '\\'; break;
        case '/': simple = '/'; break;
        case 'b': simple = '\b'; break;
        case 'f': simple = '\f'; break;
        case 'n': simple = '\n'; break;
        case 'r': simple = '\r'; break;
        case 't': simple = '\t'; break;
        case 'u': {
            unsigned long code_point = 0;
            if (!json_parse_hex4(parser, &code_point)) {
                free(buffer);
                return NULL;
            }
            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                /* High surrogate: require a following low surrogate. */
                if (parser->cursor[0] != '\\' || parser->cursor[1] != 'u') {
                    free(buffer);
                    json_set_error(parser, "Unpaired surrogate in JSON string.");
                    return NULL;
                }
                parser->cursor += 2;
                unsigned long low = 0;
                if (!json_parse_hex4(parser, &low) || low < 0xDC00 ||
                    low > 0xDFFF) {
                    free(buffer);
                    json_set_error(parser, "Unpaired surrogate in JSON string.");
                    return NULL;
                }
                code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                             (low - 0xDC00);
            } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                free(buffer);
                json_set_error(parser, "Unpaired surrogate in JSON string.");
                return NULL;
            }
            char encoded[4];
            size_t encoded_length = json_encode_utf8(encoded, code_point);
            if (length + encoded_length + 1 > capacity) {
                while (length + encoded_length + 1 > capacity)
                    capacity *= 2;
                char *resized = (char *)realloc(buffer, capacity);
                if (!resized) {
                    free(buffer);
                    json_set_error(parser, "Out of memory while parsing JSON string.");
                    return NULL;
                }
                buffer = resized;
            }
            memcpy(buffer + length, encoded, encoded_length);
            length += encoded_length;
            continue;
        }
        default:
            free(buffer);
            json_set_error(parser, "Invalid escape in JSON string.");
            return NULL;
        }
        if (simple) {
            if (length + 1 > capacity) {
                capacity *= 2;
                char *resized = (char *)realloc(buffer, capacity);
                if (!resized) {
                    free(buffer);
                    json_set_error(parser, "Out of memory while parsing JSON string.");
                    return NULL;
                }
                buffer = resized;
            }
            buffer[length++] = simple;
        }
    }
    if (*parser->cursor != '"') {
        free(buffer);
        json_set_error(parser, "Unterminated string in JSON document.");
        return NULL;
    }
    parser->cursor++;
    if (length + 1 > capacity) {
        char *resized = (char *)realloc(buffer, length + 1);
        if (!resized) {
            free(buffer);
            json_set_error(parser, "Out of memory while parsing JSON string.");
            return NULL;
        }
        buffer = resized;
    }
    buffer[length] = '\0';
    JsonValue *value = json_value_new(JSON_VALUE_STRING);
    if (!value) {
        free(buffer);
        json_set_error(parser, "Out of memory while parsing JSON string.");
        return NULL;
    }
    value->string = buffer;
    return value;
}

static void json_skip_whitespace(JsonParser *parser) {
    while (*parser->cursor == ' ' || *parser->cursor == '\t' ||
           *parser->cursor == '\n' || *parser->cursor == '\r')
        parser->cursor++;
}

static JsonValue *json_parse_value(JsonParser *parser);

static JsonValue *json_parse_array(JsonParser *parser) {
    parser->cursor++; /* past '[' */
    JsonValue *array = json_value_new(JSON_VALUE_ARRAY);
    if (!array) {
        json_set_error(parser, "Out of memory while parsing JSON array.");
        return NULL;
    }
    json_skip_whitespace(parser);
    if (*parser->cursor == ']') {
        parser->cursor++;
        return array;
    }
    for (;;) {
        JsonValue *item = json_parse_value(parser);
        if (!item) {
            json_value_free(array);
            return NULL;
        }
        JsonValue **resized = (JsonValue **)json_realloc_array(
            array->items, array->item_count + 1, sizeof(*array->items));
        if (!resized) {
            json_value_free(item);
            json_value_free(array);
            json_set_error(parser, "Out of memory while parsing JSON array.");
            return NULL;
        }
        array->items = resized;
        array->items[array->item_count++] = item;
        json_skip_whitespace(parser);
        if (*parser->cursor == ',') {
            parser->cursor++;
            json_skip_whitespace(parser);
            continue;
        }
        if (*parser->cursor == ']') {
            parser->cursor++;
            return array;
        }
        json_value_free(array);
        json_set_error(parser, "Expected ',' or ']' in JSON array.");
        return NULL;
    }
}

static JsonValue *json_parse_object(JsonParser *parser) {
    parser->cursor++; /* past '{' */
    JsonValue *object = json_value_new(JSON_VALUE_OBJECT);
    if (!object) {
        json_set_error(parser, "Out of memory while parsing JSON object.");
        return NULL;
    }
    json_skip_whitespace(parser);
    if (*parser->cursor == '}') {
        parser->cursor++;
        return object;
    }
    for (;;) {
        json_skip_whitespace(parser);
        if (*parser->cursor != '"') {
            json_value_free(object);
            json_set_error(parser, "Expected object key in JSON object.");
            return NULL;
        }
        parser->cursor++;
        JsonValue *key_value = json_parse_string(parser);
        if (!key_value) {
            json_value_free(object);
            return NULL;
        }
        json_skip_whitespace(parser);
        if (*parser->cursor != ':') {
            json_value_free(key_value);
            json_value_free(object);
            json_set_error(parser, "Expected ':' after JSON object key.");
            return NULL;
        }
        parser->cursor++;
        json_skip_whitespace(parser);
        JsonValue *member_value = json_parse_value(parser);
        if (!member_value) {
            json_value_free(key_value);
            json_value_free(object);
            return NULL;
        }
        JsonMember *resized = (JsonMember *)json_realloc_array(
            object->members, object->member_count + 1, sizeof(*object->members));
        if (!resized) {
            json_value_free(key_value);
            json_value_free(member_value);
            json_value_free(object);
            json_set_error(parser, "Out of memory while parsing JSON object.");
            return NULL;
        }
        object->members = resized;
        object->members[object->member_count].key = key_value->string;
        object->members[object->member_count].value = member_value;
        object->member_count++;
        /* The key string is owned by the member list now. */
        key_value->string = NULL;
        json_value_free(key_value);
        json_skip_whitespace(parser);
        if (*parser->cursor == ',') {
            parser->cursor++;
            continue;
        }
        if (*parser->cursor == '}') {
            parser->cursor++;
            return object;
        }
        json_value_free(object);
        json_set_error(parser, "Expected ',' or '}' in JSON object.");
        return NULL;
    }
}

static bool json_parse_literal(JsonParser *parser, const char *literal,
                               JsonValueType type, JsonValue **out) {
    size_t length = strlen(literal);
    if (strncmp(parser->cursor, literal, length) != 0)
        return false;
    parser->cursor += length;
    JsonValue *value = json_value_new(type);
    if (!value) {
        json_set_error(parser, "Out of memory while parsing JSON value.");
        return false;
    }
    *out = value;
    return true;
}

static JsonValue *json_parse_number(JsonParser *parser) {
    const char *start = parser->cursor;
    if (*parser->cursor == '-')
        parser->cursor++;
    if (*parser->cursor == '0') {
        parser->cursor++;
    } else if (*parser->cursor >= '1' && *parser->cursor <= '9') {
        while (isdigit((unsigned char)*parser->cursor))
            parser->cursor++;
    } else {
        json_set_error(parser, "Invalid number in JSON document.");
        return NULL;
    }
    if (*parser->cursor == '.') {
        parser->cursor++;
        if (!isdigit((unsigned char)*parser->cursor)) {
            json_set_error(parser, "Invalid number in JSON document.");
            return NULL;
        }
        while (isdigit((unsigned char)*parser->cursor))
            parser->cursor++;
    }
    if (*parser->cursor == 'e' || *parser->cursor == 'E') {
        parser->cursor++;
        if (*parser->cursor == '+' || *parser->cursor == '-')
            parser->cursor++;
        if (!isdigit((unsigned char)*parser->cursor)) {
            json_set_error(parser, "Invalid number in JSON document.");
            return NULL;
        }
        while (isdigit((unsigned char)*parser->cursor))
            parser->cursor++;
    }
    JsonValue *value = json_value_new(JSON_VALUE_NUMBER);
    if (!value) {
        json_set_error(parser, "Out of memory while parsing JSON number.");
        return NULL;
    }
    value->string = json_strdup_range(start, (size_t)(parser->cursor - start));
    if (!value->string) {
        json_value_free(value);
        json_set_error(parser, "Out of memory while parsing JSON number.");
        return NULL;
    }
    return value;
}

static JsonValue *json_parse_value(JsonParser *parser) {
    if (++parser->depth > JSON_MAX_DEPTH) {
        json_set_error(parser, "JSON document is nested too deeply.");
        return NULL;
    }
    json_skip_whitespace(parser);
    JsonValue *value = NULL;
    char current = *parser->cursor;
    if (current == '{') {
        value = json_parse_object(parser);
    } else if (current == '[') {
        value = json_parse_array(parser);
    } else if (current == '"') {
        parser->cursor++;
        value = json_parse_string(parser);
    } else if (current == '-' || isdigit((unsigned char)current)) {
        value = json_parse_number(parser);
    } else if (current == 't') {
        if (!json_parse_literal(parser, "true", JSON_VALUE_TRUE, &value))
            json_set_error(parser, "Invalid literal in JSON document.");
    } else if (current == 'f') {
        if (!json_parse_literal(parser, "false", JSON_VALUE_FALSE, &value))
            json_set_error(parser, "Invalid literal in JSON document.");
    } else if (current == 'n') {
        if (!json_parse_literal(parser, "null", JSON_VALUE_NULL, &value))
            json_set_error(parser, "Invalid literal in JSON document.");
    } else {
        json_set_error(parser, "Unexpected character in JSON document.");
    }
    parser->depth--;
    return value;
}

JsonValue *json_parse(const char *text, char *error_buffer,
                      size_t error_capacity) {
    if (!text) {
        if (error_buffer && error_capacity)
            snprintf(error_buffer, error_capacity, "Empty JSON input.");
        return NULL;
    }
    JsonParser parser = {.text = text, .cursor = text, .error = {0}, .depth = 0};
    JsonValue *value = json_parse_value(&parser);
    if (value) {
        json_skip_whitespace(&parser);
        if (*parser.cursor) {
            json_value_free(value);
            value = NULL;
            json_set_error(&parser, "Trailing characters after JSON document.");
        }
    }
    if (!value && error_buffer && error_capacity)
        snprintf(error_buffer, error_capacity, "%s",
                 parser.error[0] ? parser.error : "Invalid JSON document.");
    return value;
}

JsonValue *json_parse_file(const char *path, char *error_buffer,
                           size_t error_capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error_buffer && error_capacity)
            snprintf(error_buffer, error_capacity, "Cannot open '%s'.", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        if (error_buffer && error_capacity)
            snprintf(error_buffer, error_capacity, "Cannot read '%s'.", path);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        if (error_buffer && error_capacity)
            snprintf(error_buffer, error_capacity, "Cannot read '%s'.", path);
        return NULL;
    }
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) {
        fclose(file);
        if (error_buffer && error_capacity)
            snprintf(error_buffer, error_capacity, "Out of memory reading '%s'.",
                     path);
        return NULL;
    }
    size_t bytes_read = fread(text, 1, (size_t)length, file);
    bool read_failed = ferror(file) != 0;
    fclose(file);
    if (bytes_read != (size_t)length || read_failed) {
        free(text);
        if (error_buffer && error_capacity)
            snprintf(error_buffer, error_capacity, "Cannot read '%s'.", path);
        return NULL;
    }
    text[bytes_read] = '\0';
    JsonValue *value = json_parse(text, error_buffer, error_capacity);
    free(text);
    return value;
}

void json_value_free(JsonValue *value) {
    if (!value)
        return;
    free(value->string);
    for (size_t index = 0; index < value->member_count; index++) {
        free(value->members[index].key);
        json_value_free(value->members[index].value);
    }
    free(value->members);
    for (size_t index = 0; index < value->item_count; index++)
        json_value_free(value->items[index]);
    free(value->items);
    free(value);
}

const JsonValue *json_object_member(const JsonValue *object, const char *key) {
    if (!object || object->type != JSON_VALUE_OBJECT || !key)
        return NULL;
    for (size_t index = 0; index < object->member_count; index++)
        if (!strcmp(object->members[index].key, key))
            return object->members[index].value;
    return NULL;
}

static bool json_write_escaped_string(FILE *stream, const char *text) {
    if (fputc('"', stream) == EOF)
        return false;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
         cursor++) {
        const char *escape = NULL;
        char buffer[7];
        switch (*cursor) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default:
            if (*cursor < 0x20) {
                (void)snprintf(buffer, sizeof(buffer), "\\u%04x", *cursor);
                escape = buffer;
            }
            break;
        }
        if (escape) {
            if (fputs(escape, stream) == EOF)
                return false;
        } else if (fputc(*cursor, stream) == EOF) {
            return false;
        }
    }
    return fputc('"', stream) != EOF;
}

static bool json_write_value(const JsonValue *value, FILE *stream,
                             bool pretty, unsigned int depth) {
    switch (value->type) {
    case JSON_VALUE_NULL:
        return fputs("null", stream) != EOF;
    case JSON_VALUE_FALSE:
        return fputs("false", stream) != EOF;
    case JSON_VALUE_TRUE:
        return fputs("true", stream) != EOF;
    case JSON_VALUE_NUMBER:
    case JSON_VALUE_STRING:
        if (value->type == JSON_VALUE_NUMBER)
            return fputs(value->string ? value->string : "0", stream) != EOF;
        return json_write_escaped_string(stream, value->string ? value->string : "");
    case JSON_VALUE_ARRAY: {
        if (fputc('[', stream) == EOF)
            return false;
        for (size_t index = 0; index < value->item_count; index++) {
            if (index && fputc(',', stream) == EOF)
                return false;
            if (pretty && fputc('\n', stream) == EOF)
                return false;
            if (pretty)
                for (unsigned int indent = 0; indent <= depth; indent++)
                    if (fputs("  ", stream) == EOF)
                        return false;
            if (!json_write_value(value->items[index], stream, pretty, depth + 1))
                return false;
        }
        if (value->item_count && pretty && fputc('\n', stream) == EOF)
            return false;
        if (pretty && value->item_count)
            for (unsigned int indent = 0; indent < depth; indent++)
                if (fputs("  ", stream) == EOF)
                    return false;
        return fputc(']', stream) != EOF;
    }
    case JSON_VALUE_OBJECT: {
        if (fputc('{', stream) == EOF)
            return false;
        for (size_t index = 0; index < value->member_count; index++) {
            if (index && fputc(',', stream) == EOF)
                return false;
            if (pretty && fputc('\n', stream) == EOF)
                return false;
            if (pretty)
                for (unsigned int indent = 0; indent <= depth; indent++)
                    if (fputs("  ", stream) == EOF)
                        return false;
            if (!json_write_escaped_string(stream, value->members[index].key) ||
                fputs(": ", stream) == EOF ||
                !json_write_value(value->members[index].value, stream, pretty,
                                  depth + 1))
                return false;
        }
        if (value->member_count && pretty && fputc('\n', stream) == EOF)
            return false;
        if (pretty && value->member_count)
            for (unsigned int indent = 0; indent < depth; indent++)
                if (fputs("  ", stream) == EOF)
                    return false;
        return fputc('}', stream) != EOF;
    }
    }
    return false;
}

bool json_write(const JsonValue *value, FILE *stream, bool pretty) {
    if (!value || !stream)
        return false;
    if (!json_write_value(value, stream, pretty, 0))
        return false;
    return fputc('\n', stream) != EOF && !ferror(stream);
}
