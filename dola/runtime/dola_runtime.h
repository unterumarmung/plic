#ifndef DOLA_RUNTIME_H
#define DOLA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOLA_RT_ABI_VERSION 4u

typedef enum dola_status {
  DOLA_STATUS_OK = 0,
  DOLA_STATUS_CLOSED = 1,
  DOLA_STATUS_NOT_FOUND = 2,
  DOLA_STATUS_INVALID_ARGUMENT = 3,
  DOLA_STATUS_OUT_OF_MEMORY = 4,
  DOLA_STATUS_PANIC = 5,
  DOLA_STATUS_INTERNAL_ERROR = 6,
  DOLA_STATUS_INCOMPATIBLE_PROTOCOL = 7,
  DOLA_STATUS_INVALID_MESSAGE = 8,
  DOLA_STATUS_MESSAGE_TOO_LARGE = 9,
} dola_status;

typedef enum dola_value_tag {
  DOLA_VALUE_UNIT = 0,
  DOLA_VALUE_BOOL = 1,
  DOLA_VALUE_INT = 2,
  DOLA_VALUE_FLOAT = 3,
  DOLA_VALUE_OBJECT = 4,
} dola_value_tag;

typedef struct dola_object dola_object;
typedef struct dola_runtime dola_runtime;
typedef struct dola_context dola_context;

typedef union dola_value_payload {
  uint8_t boolean;
  int64_t integer;
  double floating;
  dola_object* object;
  uint64_t bits;
} dola_value_payload;

typedef struct dola_value {
  uint32_t tag;
  uint32_t reserved;
  dola_value_payload payload;
} dola_value;

typedef dola_status (*dola_task_entry)(dola_context* context,
                                       const dola_value* environment,
                                       dola_value* out_value);

typedef enum dola_map_key_kind {
  DOLA_MAP_KEY_INT = 0,
  DOLA_MAP_KEY_STRING = 1,
} dola_map_key_kind;

/* Stable operation identifiers used by compiler-generated code. */
typedef enum dola_codegen_operation {
#define DOLA_CODEGEN_OPERATION(NAME, C_NAME, VALUE, MLIR_NAME, MIN_INPUTS,      \
                               MAX_INPUTS, RESULTS, METADATA)                  \
  C_NAME = VALUE,
#include "codegen_operations.def"
#undef DOLA_CODEGEN_OPERATION
} dola_codegen_operation;

uint32_t dola_rt_abi_version(void);
size_t dola_rt_live_object_count(void);
dola_status dola_rt_create(dola_runtime** out_runtime);
void dola_rt_destroy(dola_runtime* runtime);

/* Execution contexts carry scheduler ownership and task-local panic state. */
dola_status dola_rt_context_create(dola_runtime* runtime,
                                   dola_context** out_context);
void dola_rt_context_destroy(dola_context* context);
dola_status dola_rt_context_set_panic(dola_context* context,
                                      dola_value message);
dola_status dola_rt_context_report_panic(dola_context* context);
dola_status dola_rt_codegen_context_set_panic(dola_context* context,
                                              const dola_value* message);

/* Channel handles are immutable reference-counted values. Send and close use a
 * sender handle; receive uses a receiver handle. A closed receive succeeds with
 * out_has_value set to zero. */
dola_status dola_rt_channel_create(uint64_t sender_type_id,
                                   uint64_t receiver_type_id,
                                   dola_value* out_sender,
                                   dola_value* out_receiver);
dola_status dola_rt_channel_send(dola_value sender, dola_value value);
dola_status dola_rt_channel_receive(dola_value receiver, uint8_t* out_has_value,
                                    dola_value* out_value);
dola_status dola_rt_channel_close(dola_value sender);

/* Spawn takes a borrowed environment. Join may be repeated; each successful
 * join produces a separately owned output. Panics are returned as an owned
 * string with out_panicked set to one. */
dola_status dola_rt_task_spawn(dola_context* context, dola_task_entry entry,
                               dola_value environment, uint64_t task_type_id,
                               dola_value* out_task);
dola_status dola_rt_task_join(dola_value task, uint8_t* out_panicked,
                              dola_value* out_value);

dola_status dola_rt_read_line(dola_value* out_line);
dola_status dola_rt_read_text(dola_value path, dola_value* out_contents);
dola_status dola_rt_write_text(dola_value path, dola_value contents);
dola_status dola_rt_unix_seconds(int64_t* out_seconds);
dola_status dola_rt_sleep_ms(int64_t milliseconds);
dola_status dola_rt_string_split_once(dola_value value, dola_value delimiter,
                                      uint64_t tuple_type_id,
                                      uint8_t* out_found,
                                      dola_value* out_value);
dola_status dola_rt_parse_int(dola_value value, int64_t* out_integer);

dola_status dola_rt_transport_listen(dola_value address,
                                     uint64_t request_type_id,
                                     uint64_t response_type_id,
                                     uint64_t listener_type_id,
                                     dola_value* out_listener);
dola_status dola_rt_transport_local_address(dola_value listener,
                                            dola_value* out_address);
dola_status dola_rt_transport_connect(dola_value address, uint64_t send_type_id,
                                      uint64_t receive_type_id,
                                      uint64_t connection_type_id,
                                      dola_value* out_connection);
dola_status dola_rt_transport_accept(dola_value listener,
                                     uint64_t connection_type_id,
                                     dola_value* out_connection);
dola_status dola_rt_transport_send(dola_value connection, dola_value value);
dola_status dola_rt_transport_receive(dola_value connection,
                                      uint8_t* out_has_value,
                                      dola_value* out_value);
dola_status dola_rt_transport_close(dola_value resource);

/* Legacy borrowed byte output retained for ABI compatibility. */
dola_status dola_rt_print(const uint8_t* bytes, size_t length);
dola_status dola_rt_println(const uint8_t* bytes, size_t length);
void dola_rt_panic(const uint8_t* bytes, size_t length);
const char* dola_rt_last_error_message(void);

/* Generic values are borrowed unless an owned output is documented by an
 * out_value parameter. Owned outputs must be released exactly once. */
void dola_rt_value_retain(dola_value value);
void dola_rt_value_release(dola_value value);
dola_status dola_rt_value_equal(dola_value left, dola_value right,
                                uint8_t* out_equal);

dola_status dola_rt_string_create(const uint8_t* bytes, size_t length,
                                  dola_value* out_string);
dola_status dola_rt_string_length(dola_value string, int64_t* out_length);
dola_status dola_rt_string_concat(dola_value left, dola_value right,
                                  dola_value* out_string);
dola_status dola_rt_int_to_string(int64_t value, dola_value* out_string);
dola_status dola_rt_print_value(dola_value string);
dola_status dola_rt_println_value(dola_value string);
dola_status dola_rt_eprintln_value(dola_value string);

dola_status dola_rt_tuple_create(uint64_t type_id, const dola_value* elements,
                                 size_t length, dola_value* out_tuple);
dola_status dola_rt_tuple_get(dola_value tuple, size_t index,
                              dola_value* out_value);

dola_status dola_rt_record_create(uint64_t type_id, const dola_value* fields,
                                  size_t length, dola_value* out_record);
dola_status dola_rt_record_get(dola_value record, size_t field,
                               dola_value* out_value);
dola_status dola_rt_record_with(dola_value record, size_t field,
                                dola_value value, dola_value* out_record);

dola_status dola_rt_enum_create(uint64_t type_id, uint32_t variant,
                                const dola_value* payloads, size_t length,
                                dola_value* out_enum);
dola_status dola_rt_enum_tag(dola_value value, uint32_t* out_variant);
dola_status dola_rt_enum_payload(dola_value value, size_t index,
                                 dola_value* out_value);

dola_status dola_rt_list_create(uint64_t type_id, dola_value* out_list);
dola_status dola_rt_list_length(dola_value list, int64_t* out_length);
dola_status dola_rt_list_get(dola_value list, int64_t index,
                             dola_value* out_value);
dola_status dola_rt_list_push(dola_value list, dola_value value,
                              dola_value* out_list);
dola_status dola_rt_list_set(dola_value list, int64_t index, dola_value value,
                             dola_value* out_list);
dola_status dola_rt_list_remove_at(dola_value list, int64_t index,
                                   dola_value* out_list);
dola_status dola_rt_list_take_last(dola_value list, int64_t count,
                                   dola_value* out_list);

dola_status dola_rt_map_create(uint64_t type_id, dola_map_key_kind key_kind,
                               dola_value* out_map);
dola_status dola_rt_map_length(dola_value map, int64_t* out_length);
dola_status dola_rt_map_contains(dola_value map, dola_value key,
                                 uint8_t* out_contains);
dola_status dola_rt_map_get(dola_value map, dola_value key, uint8_t* out_found,
                            dola_value* out_value);
dola_status dola_rt_map_insert(dola_value map, dola_value key, dola_value value,
                               dola_value* out_map);
dola_status dola_rt_map_remove(dola_value map, dola_value key,
                               dola_value* out_map);
dola_status dola_rt_map_entries(dola_value map, uint64_t tuple_type_id,
                                uint64_t list_type_id, dola_value* out_list);

dola_status dola_rt_codegen(uint32_t operation, uint64_t result_type_id,
                            int64_t metadata0, int64_t metadata1,
                            const dola_value* inputs, size_t input_count,
                            dola_value* out_value);
int32_t dola_rt_result_main(const dola_value* result);
void dola_rt_codegen_retain(const dola_value* value);
void dola_rt_codegen_release(const dola_value* value);
dola_status dola_rt_codegen_equal(const dola_value* left,
                                  const dola_value* right, uint8_t* out_equal);
dola_status dola_rt_codegen_print(const dola_value* value, uint8_t newline);

#ifdef __cplusplus
}
#endif
#endif
