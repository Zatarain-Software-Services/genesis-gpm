#ifndef GPM_JSON_H
#define GPM_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define JSON_ERROR_SIZE 160

typedef enum {
    JSON_VALUE_NULL,
    JSON_VALUE_FALSE,
    JSON_VALUE_TRUE,
    JSON_VALUE_NUMBER,
    JSON_VALUE_STRING,
    JSON_VALUE_ARRAY,
    JSON_VALUE_OBJECT
} JsonValueType;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonValueType type;
    /* JSON_VALUE_STRING holds the decoded text; JSON_VALUE_NUMBER holds the
       raw literal so callers can validate its shape themselves. */
    char *string;
    JsonMember *members;
    size_t member_count;
    JsonValue **items;
    size_t item_count;
};

/* Constructors for building documents. json_object_add takes ownership of
   key (copied) and value. */
JsonValue *json_value_new(JsonValueType type);
JsonValue *json_string_new(const char *text);
JsonValue *json_number_new(const char *literal);
bool json_object_add(JsonValue *object, const char *key, JsonValue *value);
/* Appends item to a JSON_VALUE_ARRAY, taking ownership. Returns false on
   allocation failure (item is freed). */
bool json_array_add(JsonValue *array, JsonValue *item);

/* Parses a complete JSON document. On failure returns NULL and writes a
   short diagnostic into error_buffer when it is non-NULL. */
JsonValue *json_parse(const char *text, char *error_buffer,
                      size_t error_capacity);
JsonValue *json_parse_file(const char *path, char *error_buffer,
                           size_t error_capacity);
void json_value_free(JsonValue *value);

/* Returns the member with the exact key, or NULL. Only meaningful for
   JSON_VALUE_OBJECT values. */
const JsonValue *json_object_member(const JsonValue *object, const char *key);

/* Serializes the value. pretty=true emits the two-space indented form used
   for genesis.json and genesis.lock.json. Returns false on allocation or
   stream failure. */
bool json_write(const JsonValue *value, FILE *stream, bool pretty);

#endif
