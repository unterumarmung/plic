#include "dola_runtime.h"

#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
dola_value integer(int64_t value) {
  dola_value result{};
  result.tag = DOLA_VALUE_INT;
  result.payload.integer = value;
  return result;
}

dola_value invalidValue() {
  dola_value result{};
  result.tag = 99;
  return result;
}

dola_value nullObject() {
  dola_value result{};
  result.tag = DOLA_VALUE_OBJECT;
  result.payload.object = nullptr;
  return result;
}

dola_value makeString(const std::string& value) {
  dola_value result{};
  const std::vector<uint8_t> bytes(value.begin(), value.end());
  EXPECT_EQ(dola_rt_string_create(bytes.data(), bytes.size(), &result),
            DOLA_STATUS_OK);
  return result;
}

void expectInvalid(dola_status status) {
  EXPECT_EQ(status, DOLA_STATUS_INVALID_ARGUMENT);
  ASSERT_NE(dola_rt_last_error_message(), nullptr);
  EXPECT_NE(std::string(dola_rt_last_error_message()).size(), 0U);
}

void expectStringEquals(dola_value actual, const std::string& expected) {
  dola_value expectedValue = makeString(expected);
  uint8_t equal = 0;
  ASSERT_EQ(dola_rt_value_equal(actual, expectedValue, &equal),
            DOLA_STATUS_OK);
  EXPECT_EQ(equal, 1);
  dola_rt_value_release(expectedValue);
}

dola_status echoTask(dola_context*, const dola_value* environment,
                     dola_value* output) {
  *output = *environment;
  dola_rt_value_retain(*environment);
  return DOLA_STATUS_OK;
}

dola_status panicTask(dola_context* context, const dola_value*,
                      dola_value* output) {
  *output = {};
  dola_value message = makeString("contained panic");
  const dola_status status = dola_rt_context_set_panic(context, message);
  dola_rt_value_release(message);
  return status == DOLA_STATUS_OK ? DOLA_STATUS_PANIC : status;
}
} // namespace

TEST(RuntimeAbiTest, ReportsVersionAndManagesRuntime) {
  EXPECT_EQ(dola_rt_abi_version(), DOLA_RT_ABI_VERSION);
  EXPECT_EQ(DOLA_CODEGEN_PANIC, 45);
  EXPECT_EQ(DOLA_CODEGEN_STRING_CONCAT, 46);
  EXPECT_EQ(DOLA_CODEGEN_INT_TO_STRING, 47);
  dola_runtime* runtime = nullptr;
  ASSERT_EQ(dola_rt_create(&runtime), DOLA_STATUS_OK);
  ASSERT_NE(runtime, nullptr);
  dola_rt_destroy(runtime);
  dola_rt_destroy(nullptr);
}

TEST(RuntimeAbiTest, ConcatenatesOwnedUtf8AndFormatsEveryIntegerBoundary) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value empty = makeString("");
  dola_value left = makeString("hello ");
  dola_value right = makeString("UTF-8 λ");
  dola_value result{};
  ASSERT_EQ(dola_rt_string_concat(left, right, &result), DOLA_STATUS_OK);
  expectStringEquals(result, "hello UTF-8 λ");
  dola_rt_value_release(result);
  ASSERT_EQ(dola_rt_string_concat(empty, right, &result), DOLA_STATUS_OK);
  expectStringEquals(result, "UTF-8 λ");
  dola_rt_value_release(result);

  const std::string large(8192, 'x');
  dola_value largeValue = makeString(large);
  ASSERT_EQ(dola_rt_string_concat(largeValue, largeValue, &result),
            DOLA_STATUS_OK);
  int64_t length = 0;
  ASSERT_EQ(dola_rt_string_length(result, &length), DOLA_STATUS_OK);
  EXPECT_EQ(length, 16384);
  dola_rt_value_release(result);

  const std::array<int64_t, 5> boundaries{
      std::numeric_limits<int64_t>::min(), -1, 0, 1,
      std::numeric_limits<int64_t>::max()};
  for (const int64_t value : boundaries) {
    ASSERT_EQ(dola_rt_int_to_string(value, &result), DOLA_STATUS_OK);
    expectStringEquals(result, std::to_string(value));
    dola_rt_value_release(result);
  }

  result = integer(99);
  expectInvalid(dola_rt_string_concat(integer(1), right, &result));
  EXPECT_EQ(result.tag, DOLA_VALUE_INT);
  EXPECT_EQ(result.payload.integer, 99);
  expectInvalid(dola_rt_string_concat(left, right, nullptr));
  expectInvalid(dola_rt_int_to_string(1, nullptr));
  dola_rt_value_release(largeValue);
  dola_rt_value_release(right);
  dola_rt_value_release(left);
  dola_rt_value_release(empty);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, ContextPanicAdaptersRejectNullPointers) {
  dola_value message = makeString("panic");
  expectInvalid(dola_rt_context_set_panic(nullptr, message));
  expectInvalid(dola_rt_codegen_context_set_panic(nullptr, &message));
  expectInvalid(dola_rt_context_report_panic(nullptr));
  dola_runtime* runtime = nullptr;
  ASSERT_EQ(dola_rt_create(&runtime), DOLA_STATUS_OK);
  dola_context* context = nullptr;
  ASSERT_EQ(dola_rt_context_create(runtime, &context), DOLA_STATUS_OK);
  expectInvalid(dola_rt_codegen_context_set_panic(context, nullptr));
  dola_rt_context_destroy(context);
  dola_rt_destroy(runtime);
  dola_rt_value_release(message);
}

TEST(RuntimeAbiTest, ChannelsQueueValuesCloseAndValidateKinds) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value sender{};
  dola_value receiver{};
  ASSERT_EQ(dola_rt_channel_create(101, 102, &sender, &receiver),
            DOLA_STATUS_OK);
  ASSERT_EQ(dola_rt_channel_send(sender, integer(42)), DOLA_STATUS_OK);
  uint8_t hasValue = 0;
  dola_value received{};
  ASSERT_EQ(dola_rt_channel_receive(receiver, &hasValue, &received),
            DOLA_STATUS_OK);
  EXPECT_EQ(hasValue, 1);
  EXPECT_EQ(received.payload.integer, 42);
  dola_rt_value_release(received);
  ASSERT_EQ(dola_rt_channel_close(sender), DOLA_STATUS_OK);
  ASSERT_EQ(dola_rt_channel_receive(receiver, &hasValue, &received),
            DOLA_STATUS_OK);
  EXPECT_EQ(hasValue, 0);
  EXPECT_EQ(dola_rt_channel_send(sender, integer(1)), DOLA_STATUS_CLOSED);
  expectInvalid(dola_rt_channel_send(receiver, integer(1)));
  dola_rt_value_release(receiver);
  dola_rt_value_release(sender);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, TasksJoinRepeatedlyAndContainPanics) {
  const size_t baseline = dola_rt_live_object_count();
  dola_runtime* runtime = nullptr;
  ASSERT_EQ(dola_rt_create(&runtime), DOLA_STATUS_OK);
  dola_context* context = nullptr;
  ASSERT_EQ(dola_rt_context_create(runtime, &context), DOLA_STATUS_OK);

  dola_value task{};
  ASSERT_EQ(dola_rt_task_spawn(context, echoTask, integer(17), 201, &task),
            DOLA_STATUS_OK);
  for (int iteration = 0; iteration < 2; ++iteration) {
    uint8_t panicked = 1;
    dola_value result{};
    ASSERT_EQ(dola_rt_task_join(task, &panicked, &result), DOLA_STATUS_OK);
    EXPECT_EQ(panicked, 0);
    EXPECT_EQ(result.payload.integer, 17);
    dola_rt_value_release(result);
  }
  dola_rt_value_release(task);

  ASSERT_EQ(dola_rt_task_spawn(context, panicTask, {}, 202, &task),
            DOLA_STATUS_OK);
  uint8_t panicked = 0;
  dola_value message{};
  ASSERT_EQ(dola_rt_task_join(task, &panicked, &message), DOLA_STATUS_OK);
  EXPECT_EQ(panicked, 1);
  int64_t length = 0;
  ASSERT_EQ(dola_rt_string_length(message, &length), DOLA_STATUS_OK);
  EXPECT_GT(length, 0);
  dola_rt_value_release(message);
  dola_rt_value_release(task);
  dola_rt_context_destroy(context);
  dola_rt_destroy(runtime);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, StandardTextTimeAndParsingFunctionsValidateInputs) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dola-runtime-text-test.txt";
  dola_value pathValue = makeString(path.string());
  dola_value contents = makeString("hello UTF-8: \xE2\x98\x83");
  ASSERT_EQ(dola_rt_write_text(pathValue, contents), DOLA_STATUS_OK);
  dola_value loaded{};
  ASSERT_EQ(dola_rt_read_text(pathValue, &loaded), DOLA_STATUS_OK);
  uint8_t equal = 0;
  ASSERT_EQ(dola_rt_value_equal(contents, loaded, &equal), DOLA_STATUS_OK);
  EXPECT_EQ(equal, 1);

  dola_value number = makeString("-9223372036854775808");
  int64_t parsed = 0;
  EXPECT_EQ(dola_rt_parse_int(number, &parsed), DOLA_STATUS_OK);
  EXPECT_EQ(parsed, std::numeric_limits<int64_t>::min());
  dola_value invalid = makeString("12x");
  EXPECT_EQ(dola_rt_parse_int(invalid, &parsed), DOLA_STATUS_INVALID_ARGUMENT);
  int64_t seconds = 0;
  EXPECT_EQ(dola_rt_unix_seconds(&seconds), DOLA_STATUS_OK);
  EXPECT_GT(seconds, 0);
  EXPECT_EQ(dola_rt_sleep_ms(-1), DOLA_STATUS_INVALID_ARGUMENT);

  dola_rt_value_release(invalid);
  dola_rt_value_release(number);
  dola_rt_value_release(loaded);
  dola_rt_value_release(contents);
  dola_rt_value_release(pathValue);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(RuntimeAbiTest, StringSplitOncePreservesMissingOutputAndValidatesKinds) {
  dola_value text = makeString("left:right");
  dola_value delimiter = makeString(":");
  uint8_t found = 0;
  dola_value parts{};
  ASSERT_EQ(dola_rt_string_split_once(text, delimiter, 501, &found, &parts),
            DOLA_STATUS_OK);
  EXPECT_EQ(found, 1);
  dola_value left{};
  dola_value right{};
  ASSERT_EQ(dola_rt_tuple_get(parts, 0, &left), DOLA_STATUS_OK);
  ASSERT_EQ(dola_rt_tuple_get(parts, 1, &right), DOLA_STATUS_OK);
  int64_t length = 0;
  EXPECT_EQ(dola_rt_string_length(left, &length), DOLA_STATUS_OK);
  EXPECT_EQ(length, 4);
  EXPECT_EQ(
      dola_rt_string_split_once(integer(1), delimiter, 501, &found, &parts),
      DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(dola_rt_string_split_once(text, delimiter, 501, nullptr, &parts),
            DOLA_STATUS_INVALID_ARGUMENT);
  dola_rt_value_release(right);
  dola_rt_value_release(left);
  dola_rt_value_release(parts);
  dola_rt_value_release(delimiter);
  dola_rt_value_release(text);
}

TEST(RuntimeAbiTest, TypedTransportEchoUsesEphemeralLoopbackPort) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value bindAddress = makeString("127.0.0.1:0");
  dola_value listener{};
  ASSERT_EQ(dola_rt_transport_listen(bindAddress, 0, 0, 301, &listener),
            DOLA_STATUS_OK);
  dola_value address{};
  ASSERT_EQ(dola_rt_transport_local_address(listener, &address),
            DOLA_STATUS_OK);

  std::thread server([listener] {
    dola_value connection{};
    ASSERT_EQ(dola_rt_transport_accept(listener, 302, &connection),
              DOLA_STATUS_OK);
    uint8_t hasValue = 0;
    dola_value request{};
    ASSERT_EQ(dola_rt_transport_receive(connection, &hasValue, &request),
              DOLA_STATUS_OK);
    ASSERT_EQ(hasValue, 1);
    ASSERT_EQ(dola_rt_transport_send(connection, request), DOLA_STATUS_OK);
    dola_rt_value_release(request);
    EXPECT_EQ(dola_rt_transport_close(connection), DOLA_STATUS_OK);
    dola_rt_value_release(connection);
  });

  dola_value client{};
  ASSERT_EQ(dola_rt_transport_connect(address, 0, 0, 303, &client),
            DOLA_STATUS_OK);
  dola_value message = makeString("echo");
  ASSERT_EQ(dola_rt_transport_send(client, message), DOLA_STATUS_OK);
  uint8_t hasValue = 0;
  dola_value response{};
  ASSERT_EQ(dola_rt_transport_receive(client, &hasValue, &response),
            DOLA_STATUS_OK);
  ASSERT_EQ(hasValue, 1);
  uint8_t equal = 0;
  ASSERT_EQ(dola_rt_value_equal(message, response, &equal), DOLA_STATUS_OK);
  EXPECT_EQ(equal, 1);
  dola_rt_value_release(response);
  dola_rt_value_release(message);
  EXPECT_EQ(dola_rt_transport_close(client), DOLA_STATUS_OK);
  dola_rt_value_release(client);
  server.join();
  EXPECT_EQ(dola_rt_transport_close(listener), DOLA_STATUS_OK);
  dola_rt_value_release(address);
  dola_rt_value_release(listener);
  dola_rt_value_release(bindAddress);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, TransportRejectsNullOutputsAndWrongResourceKinds) {
  dola_value address = makeString("127.0.0.1:0");
  EXPECT_EQ(dola_rt_transport_listen(address, 1, 2, 3, nullptr),
            DOLA_STATUS_INVALID_ARGUMENT);
  dola_value output{};
  EXPECT_EQ(dola_rt_transport_local_address(address, &output),
            DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(dola_rt_transport_send(address, integer(1)),
            DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(dola_rt_transport_receive(address, nullptr, &output),
            DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(dola_rt_transport_close(address), DOLA_STATUS_INVALID_ARGUMENT);
  dola_rt_value_release(address);
}

TEST(RuntimeAbiTest, GeneratedTransportErrorsUseDeclaredVariantTags) {
  dola_value address = makeString("invalid transport address");
  const std::array<dola_value, 3> inputs{address, integer(11), integer(22)};
  dola_value result{};
  ASSERT_EQ(dola_rt_codegen(DOLA_CODEGEN_TRANSPORT_CONNECT, 701, 0, 0,
                            inputs.data(), inputs.size(), &result),
            DOLA_STATUS_OK);
  uint32_t resultTag = 0;
  ASSERT_EQ(dola_rt_enum_tag(result, &resultTag), DOLA_STATUS_OK);
  EXPECT_EQ(resultTag, 1U);
  dola_value error{};
  ASSERT_EQ(dola_rt_enum_payload(result, 0, &error), DOLA_STATUS_OK);
  uint32_t errorTag = 0;
  ASSERT_EQ(dola_rt_enum_tag(error, &errorTag), DOLA_STATUS_OK);
  EXPECT_EQ(errorTag, 6U);
  dola_value message{};
  ASSERT_EQ(dola_rt_enum_payload(error, 0, &message), DOLA_STATUS_OK);
  int64_t length = 0;
  ASSERT_EQ(dola_rt_string_length(message, &length), DOLA_STATUS_OK);
  EXPECT_GT(length, 0);
  dola_rt_value_release(message);
  dola_rt_value_release(error);
  dola_rt_value_release(result);
  dola_rt_value_release(address);
}

TEST(RuntimeAbiTest, ValidatesNullPointersAndReportsLastError) {
  EXPECT_EQ(dola_rt_create(nullptr), DOLA_STATUS_INVALID_ARGUMENT);
  ASSERT_NE(dola_rt_last_error_message(), nullptr);
  EXPECT_NE(std::string(dola_rt_last_error_message()).find("out_runtime"),
            std::string::npos);
  EXPECT_EQ(dola_rt_print(nullptr, 1), DOLA_STATUS_INVALID_ARGUMENT);
}

TEST(RuntimeAbiTest, RejectsInvalidUtf8) {
  const std::array<uint8_t, 1> invalid{0xff};
  EXPECT_EQ(dola_rt_print(invalid.data(), invalid.size()),
            DOLA_STATUS_INTERNAL_ERROR);
  ASSERT_NE(dola_rt_last_error_message(), nullptr);
  EXPECT_NE(std::string(dola_rt_last_error_message()).find("UTF-8"),
            std::string::npos);
}

TEST(RuntimeAbiTest, ImmutableCollectionsPreserveSourcesAndReleaseObjects) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value list{};
  ASSERT_EQ(dola_rt_list_create(11, &list), DOLA_STATUS_OK);
  dola_value updated{};
  ASSERT_EQ(dola_rt_list_push(list, integer(7), &updated), DOLA_STATUS_OK);
  int64_t originalLength = -1;
  int64_t updatedLength = -1;
  EXPECT_EQ(dola_rt_list_length(list, &originalLength), DOLA_STATUS_OK);
  EXPECT_EQ(dola_rt_list_length(updated, &updatedLength), DOLA_STATUS_OK);
  EXPECT_EQ(originalLength, 0);
  EXPECT_EQ(updatedLength, 1);
  dola_value item{};
  EXPECT_EQ(dola_rt_list_get(updated, -1, &item), DOLA_STATUS_NOT_FOUND);
  EXPECT_EQ(dola_rt_list_get(updated, 1, &item), DOLA_STATUS_NOT_FOUND);
  ASSERT_EQ(dola_rt_list_get(updated, 0, &item), DOLA_STATUS_OK);
  EXPECT_EQ(item.payload.integer, 7);
  dola_rt_value_release(item);
  dola_rt_value_release(updated);
  dola_rt_value_release(list);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, MapsUseContentKeysAndRecursiveEquality) {
  const std::array<uint8_t, 1> keyBytes{'k'};
  dola_value firstKey{};
  dola_value secondKey{};
  ASSERT_EQ(dola_rt_string_create(keyBytes.data(), keyBytes.size(), &firstKey),
            DOLA_STATUS_OK);
  ASSERT_EQ(dola_rt_string_create(keyBytes.data(), keyBytes.size(), &secondKey),
            DOLA_STATUS_OK);
  dola_value map{};
  ASSERT_EQ(dola_rt_map_create(19, DOLA_MAP_KEY_STRING, &map), DOLA_STATUS_OK);
  dola_value inserted{};
  ASSERT_EQ(dola_rt_map_insert(map, firstKey, integer(9), &inserted),
            DOLA_STATUS_OK);
  uint8_t contains = 0;
  EXPECT_EQ(dola_rt_map_contains(inserted, secondKey, &contains),
            DOLA_STATUS_OK);
  EXPECT_EQ(contains, 1);
  uint8_t equal = 0;
  EXPECT_EQ(dola_rt_value_equal(inserted, inserted, &equal), DOLA_STATUS_OK);
  EXPECT_EQ(equal, 1);
  dola_rt_value_release(inserted);
  dola_rt_value_release(map);
  dola_rt_value_release(secondKey);
  dola_rt_value_release(firstKey);
}

TEST(RuntimeAbiTest, AggregateFunctionsValidateKindsBoundsAndOutputs) {
  const size_t baseline = dola_rt_live_object_count();
  const std::array elements{integer(1), integer(2)};
  dola_value tuple{};
  ASSERT_EQ(dola_rt_tuple_create(31, elements.data(), elements.size(), &tuple),
            DOLA_STATUS_OK);
  dola_value value{};
  EXPECT_EQ(dola_rt_tuple_get(tuple, 2, &value), DOLA_STATUS_NOT_FOUND);
  EXPECT_EQ(dola_rt_record_get(tuple, 0, &value), DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(dola_rt_tuple_get(tuple, 0, nullptr), DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(dola_rt_enum_tag(tuple, nullptr), DOLA_STATUS_INVALID_ARGUMENT);
  dola_rt_value_release(tuple);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, ListBoundariesReturnDocumentedStatuses) {
  dola_value list{};
  ASSERT_EQ(dola_rt_list_create(41, &list), DOLA_STATUS_OK);
  dola_value output{};
  EXPECT_EQ(dola_rt_list_set(list, -1, integer(0), &output),
            DOLA_STATUS_NOT_FOUND);
  EXPECT_EQ(dola_rt_list_remove_at(list, 0, &output), DOLA_STATUS_NOT_FOUND);
  EXPECT_EQ(dola_rt_list_take_last(list, 0, &output), DOLA_STATUS_OK);
  dola_rt_value_release(output);
  dola_rt_value_release(list);
}

TEST(RuntimeAbiTest, GenericValuesRejectInvalidRepresentationsAndNullOutputs) {
  uint8_t equal = 17;
  expectInvalid(dola_rt_value_equal(invalidValue(), integer(1), &equal));
  EXPECT_EQ(equal, 17);
  expectInvalid(dola_rt_value_equal(nullObject(), nullObject(), &equal));
  EXPECT_EQ(equal, 17);
  expectInvalid(dola_rt_value_equal(integer(1), integer(1), nullptr));

  // Retain and release are intentionally total for generated cleanup paths.
  dola_rt_value_retain(invalidValue());
  dola_rt_value_release(invalidValue());
  dola_rt_value_retain(nullObject());
  dola_rt_value_release(nullObject());
}

TEST(RuntimeAbiTest, StringFunctionsValidatePointersUtf8KindsAndOutputs) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value output = integer(91);
  expectInvalid(dola_rt_string_create(nullptr, 1, &output));
  EXPECT_EQ(output.payload.integer, 91);
  expectInvalid(dola_rt_string_create(nullptr, 0, nullptr));

  const std::array<uint8_t, 2> invalid{0xc3, 0x28};
  EXPECT_EQ(dola_rt_string_create(invalid.data(), invalid.size(), &output),
            DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(output.payload.integer, 91);

  dola_value string = makeString("text");
  int64_t length = -1;
  EXPECT_EQ(dola_rt_string_length(string, &length), DOLA_STATUS_OK);
  EXPECT_EQ(length, 4);
  expectInvalid(dola_rt_string_length(integer(4), &length));
  expectInvalid(dola_rt_string_length(string, nullptr));
  expectInvalid(dola_rt_print_value(integer(1)));
  expectInvalid(dola_rt_println_value(nullObject()));
  expectInvalid(dola_rt_eprintln_value(invalidValue()));
  dola_rt_value_release(string);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, AggregateFunctionsRejectEveryInvalidShape) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value output = integer(73);
  expectInvalid(dola_rt_tuple_create(1, nullptr, 1, &output));
  EXPECT_EQ(output.payload.integer, 73);
  expectInvalid(dola_rt_record_create(1, nullptr, 1, &output));
  expectInvalid(dola_rt_enum_create(1, 0, nullptr, 1, &output));
  expectInvalid(dola_rt_tuple_create(1, nullptr, 0, nullptr));

  const std::array values{integer(1), integer(2)};
  dola_value tuple{};
  dola_value record{};
  dola_value enumeration{};
  ASSERT_EQ(dola_rt_tuple_create(1, values.data(), values.size(), &tuple),
            DOLA_STATUS_OK);
  ASSERT_EQ(dola_rt_record_create(2, values.data(), values.size(), &record),
            DOLA_STATUS_OK);
  ASSERT_EQ(
      dola_rt_enum_create(3, 4, values.data(), values.size(), &enumeration),
      DOLA_STATUS_OK);

  expectInvalid(dola_rt_tuple_get(record, 0, &output));
  EXPECT_EQ(dola_rt_tuple_get(tuple, 9, &output), DOLA_STATUS_NOT_FOUND);
  expectInvalid(dola_rt_record_get(tuple, 0, &output));
  EXPECT_EQ(dola_rt_record_get(record, 9, &output), DOLA_STATUS_NOT_FOUND);
  expectInvalid(dola_rt_record_with(tuple, 0, integer(3), &output));
  EXPECT_EQ(dola_rt_record_with(record, 9, integer(3), &output),
            DOLA_STATUS_NOT_FOUND);
  expectInvalid(dola_rt_record_with(record, 0, integer(3), nullptr));

  uint32_t tag = 0;
  expectInvalid(dola_rt_enum_tag(tuple, &tag));
  expectInvalid(dola_rt_enum_tag(enumeration, nullptr));
  EXPECT_EQ(dola_rt_enum_payload(enumeration, 9, &output),
            DOLA_STATUS_INVALID_ARGUMENT);
  expectInvalid(dola_rt_enum_payload(record, 0, &output));
  expectInvalid(dola_rt_enum_payload(enumeration, 0, nullptr));

  dola_rt_value_release(enumeration);
  dola_rt_value_release(record);
  dola_rt_value_release(tuple);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, ListFunctionsRejectKindsOutputsAndInvalidBounds) {
  const size_t baseline = dola_rt_live_object_count();
  expectInvalid(dola_rt_list_create(1, nullptr));
  dola_value list{};
  ASSERT_EQ(dola_rt_list_create(1, &list), DOLA_STATUS_OK);
  dola_value output = integer(55);
  int64_t length = 55;
  expectInvalid(dola_rt_list_length(integer(0), &length));
  EXPECT_EQ(length, 55);
  expectInvalid(dola_rt_list_length(list, nullptr));
  expectInvalid(dola_rt_list_get(integer(0), 0, &output));
  expectInvalid(dola_rt_list_get(list, 0, nullptr));
  expectInvalid(dola_rt_list_push(integer(0), integer(1), &output));
  expectInvalid(dola_rt_list_push(list, integer(1), nullptr));
  expectInvalid(dola_rt_list_set(integer(0), 0, integer(1), &output));
  expectInvalid(dola_rt_list_set(list, 0, integer(1), nullptr));
  expectInvalid(dola_rt_list_remove_at(integer(0), 0, &output));
  expectInvalid(dola_rt_list_remove_at(list, 0, nullptr));
  expectInvalid(dola_rt_list_take_last(integer(0), 0, &output));
  expectInvalid(dola_rt_list_take_last(list, 0, nullptr));
  EXPECT_EQ(dola_rt_list_set(list, -1, integer(1), &output),
            DOLA_STATUS_NOT_FOUND);
  EXPECT_EQ(output.payload.integer, 55);
  EXPECT_EQ(dola_rt_list_take_last(list, -1, &output),
            DOLA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(output.payload.integer, 55);
  dola_rt_value_release(list);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, MapFunctionsRejectKindsKeysAndNullOutputs) {
  const size_t baseline = dola_rt_live_object_count();
  dola_value map{};
  // Invalid C enum discriminants can arrive across the ABI even though C++
  // itself cannot construct one without an explicit conversion.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto invalidKind = static_cast<dola_map_key_kind>(99);
  expectInvalid(dola_rt_map_create(1, invalidKind, &map));
  expectInvalid(dola_rt_map_create(1, DOLA_MAP_KEY_INT, nullptr));
  ASSERT_EQ(dola_rt_map_create(1, DOLA_MAP_KEY_INT, &map), DOLA_STATUS_OK);
  dola_value stringKey = makeString("key");
  dola_value output = integer(44);
  int64_t length = 44;
  uint8_t flag = 44;

  expectInvalid(dola_rt_map_length(integer(0), &length));
  expectInvalid(dola_rt_map_length(map, nullptr));
  expectInvalid(dola_rt_map_contains(map, stringKey, &flag));
  EXPECT_EQ(flag, 44);
  expectInvalid(dola_rt_map_contains(map, integer(0), nullptr));
  expectInvalid(dola_rt_map_get(map, stringKey, &flag, &output));
  expectInvalid(dola_rt_map_get(map, integer(0), nullptr, &output));
  expectInvalid(dola_rt_map_get(map, integer(0), &flag, nullptr));
  expectInvalid(dola_rt_map_insert(map, stringKey, integer(1), &output));
  expectInvalid(dola_rt_map_insert(map, integer(0), integer(1), nullptr));
  expectInvalid(dola_rt_map_remove(map, stringKey, &output));
  expectInvalid(dola_rt_map_remove(map, integer(0), nullptr));
  expectInvalid(dola_rt_map_entries(integer(0), 2, 3, &output));
  expectInvalid(dola_rt_map_entries(map, 2, 3, nullptr));

  EXPECT_EQ(dola_rt_map_get(map, integer(9), &flag, &output), DOLA_STATUS_OK);
  EXPECT_EQ(flag, 0);
  EXPECT_EQ(output.payload.integer, 44);
  dola_rt_value_release(stringKey);
  dola_rt_value_release(map);
  EXPECT_EQ(dola_rt_live_object_count(), baseline);
}

TEST(RuntimeAbiTest, GeneratedAdaptersRejectNullInputsWithoutCrashing) {
  uint8_t equal = 31;
  dola_value one = integer(1);
  expectInvalid(dola_rt_codegen_equal(nullptr, &one, &equal));
  EXPECT_EQ(equal, 31);
  expectInvalid(dola_rt_codegen_equal(&one, nullptr, &equal));
  expectInvalid(dola_rt_codegen_equal(&one, &one, nullptr));
  expectInvalid(dola_rt_codegen_print(nullptr, 0));
  EXPECT_EQ(dola_rt_result_main(nullptr), 1);
  dola_rt_codegen_retain(nullptr);
  dola_rt_codegen_release(nullptr);
}
