#include "dola/diagnostic.h"
#include "dola/module_graph.h"
#include "dola/parser.h"
#include "dola/sema.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace dola {
namespace {

std::optional<SemanticResult> analyzeSources(
    std::initializer_list<std::pair<std::filesystem::path, std::string>>
        sources,
    DiagnosticEngine& diagnostics, bool requireEntry = false) {
  std::vector<Module> modules;
  for (const auto& [path, source] : sources) {
    auto module = parseSource(path, source, diagnostics);
    if (!module)
      return std::nullopt;
    modules.push_back(std::move(*module));
  }
  return analyze(modules, diagnostics, requireEntry);
}

TEST(SemanticAnalyzerTest, TypesEveryAcceptedExpression) {
  DiagnosticEngine diagnostics;
  auto result = analyzeSources(
      {{"main.dola",
        "module main; fn add(a: Int, b: Int) -> Int { a + b } "
        "fn main() -> Int { var answer = add(20, 22); answer = answer + 1; "
        "if answer == 43 { 0 } else { 1 } }"}},
      diagnostics, true);
  ASSERT_TRUE(result);
  if (!result)
    return;
  EXPECT_FALSE(diagnostics.hasErrors());
  EXPECT_EQ(result->expressionTypes.size(), 16U);
  EXPECT_FALSE(result->references.empty());
  EXPECT_EQ(result->assignments.size(), 1U);
  EXPECT_TRUE(std::ranges::any_of(result->references, [](const auto& entry) {
    return std::holds_alternative<FunctionReference>(entry.second);
  }));
  EXPECT_TRUE(result->entryPoint.has_value());
  if (!result->entryPoint)
    return;
  EXPECT_EQ(result->entryPoint->module, ModuleId{0});
}

TEST(ModuleGraphTest, ResolvesPublicTransitiveModuleCall) {
  DiagnosticEngine diagnostics;
  auto result = analyzeSources(
      {{"math.dola", "module library.math; pub fn answer() -> Int { 42 }"},
       {"app.dola", "module app; use library.math; fn main() -> Int { "
                    "math.answer() }"}},
      diagnostics, true);
  ASSERT_TRUE(result);
  EXPECT_FALSE(diagnostics.hasErrors());
}

TEST(ModuleGraphTest, ExposesTypedFunctionAndImportReferences) {
  DiagnosticEngine diagnostics;
  std::vector<Module> modules;
  for (const auto& [path, source] :
       {std::pair{"library.dola",
                  "module library.math; pub fn answer() -> Int { 42 }"},
        std::pair{"app.dola",
                  "module app; use library.math; fn main() -> Int { "
                  "math.answer() }"}}) {
    auto module = parseSource(path, source, diagnostics);
    ASSERT_TRUE(module);
    if (!module)
      return;
    modules.push_back(std::move(module.value()));
  }
  sema::detail::ModuleGraph graph(modules, diagnostics);
  graph.build();
  const auto import = graph.findImport(ModuleId{1}, "math");
  ASSERT_NE(import, nullptr);
  ASSERT_TRUE(import->has_value());
  if (import == nullptr || !import->has_value())
    return;
  ASSERT_TRUE(std::holds_alternative<ModuleId>(import->value()));
  EXPECT_EQ(std::get<ModuleId>(import->value()), ModuleId{0});
  const auto function = graph.findFunction(ModuleId{0}, "answer");
  ASSERT_TRUE(function);
  if (!function)
    return;
  EXPECT_EQ(function->module, ModuleId{0});
  EXPECT_EQ(function->function, FunctionIndex{0});
}

TEST(ModuleGraphTest, RejectsMissingImportAndCycle) {
  DiagnosticEngine missingDiagnostics;
  EXPECT_FALSE(analyzeSources(
      {{"missing.dola", "module missing; use absent.pkg; fn value() {}"}},
      missingDiagnostics));
  DiagnosticEngine cycleDiagnostics;
  EXPECT_FALSE(
      analyzeSources({{"a.dola", "module cycle.a; use cycle.b; fn a() {}"},
                      {"b.dola", "module cycle.b; use cycle.a; fn b() {}"}},
                     cycleDiagnostics));
}

TEST(SemanticAnalyzerTest, RejectsPrivateAccessAndImmutableAssignment) {
  DiagnosticEngine visibilityDiagnostics;
  EXPECT_FALSE(analyzeSources(
      {{"lib.dola", "module private.lib; fn hidden() -> Int { 1 }"},
       {"user.dola", "module private.user; use private.lib; "
                     "fn main() -> Int { lib.hidden() }"}},
      visibilityDiagnostics, true));
  DiagnosticEngine mutabilityDiagnostics;
  EXPECT_FALSE(analyzeSources(
      {{"main.dola", "module main; fn main() { let value = 1; value = 2; }"}},
      mutabilityDiagnostics, true));
}

TEST(SemanticAnalyzerTest, RejectsInvalidDeclarationsAndExpressions) {
  const std::vector<std::string> invalidSources = {
      "module duplicate; fn value() {} fn value() {}",
      "module parameter; fn value(item: Int, item: Int) {}",
      "module before_use; fn main() { let answer = missing; let missing = 1; }",
      "module arity; fn value(item: Int) {} fn main() { value(); }",
      "module call_type; fn value(item: Int) {} fn main() { value(true); }",
      "module condition; fn main() { if 1 {} }",
      "module branch; fn main() -> Int { if true { 1 } else { false } }",
      "module returned; fn main() -> Int { return false; }",
      "module entry; fn main(value: Int) {}",
  };
  for (size_t index = 0; index < invalidSources.size(); ++index) {
    DiagnosticEngine diagnostics;
    SCOPED_TRACE(index);
    EXPECT_FALSE(analyzeSources({{"invalid.dola", invalidSources[index]}},
                                diagnostics, true));
    EXPECT_TRUE(diagnostics.hasErrors());
  }
}

TEST(ModuleGraphTest, RejectsDuplicateModulesAndImportAliases) {
  DiagnosticEngine moduleDiagnostics;
  EXPECT_FALSE(
      analyzeSources({{"first.dola", "module duplicate; fn first() {}"},
                      {"second.dola", "module duplicate; fn second() {}"}},
                     moduleDiagnostics));

  DiagnosticEngine aliasDiagnostics;
  EXPECT_FALSE(analyzeSources(
      {{"left.dola", "module left.math; fn left() {}"},
       {"right.dola", "module right.math; fn right() {}"},
       {"user.dola",
        "module user; use left.math; use right.math; fn use() {}"}},
      aliasDiagnostics));
}

TEST(ModuleGraphTest, OrdersCrossFileDiagnosticsByDeclaredSourceOrder) {
  DiagnosticEngine diagnostics;
  EXPECT_FALSE(analyzeSources(
      {{"second.dola", "module second; fn bad() -> Int { false }"},
       {"first.dola", "module first; fn bad() -> Bool { 1 }"}},
      diagnostics));
  ASSERT_GE(diagnostics.diagnostics().size(), 2U);
  EXPECT_TRUE(std::ranges::any_of(diagnostics.diagnostics(), [](const auto& d) {
    return d.location.path == "second.dola" && d.location.line == 1 &&
           d.location.column > 1;
  }));
  EXPECT_TRUE(std::ranges::any_of(diagnostics.diagnostics(), [](const auto& d) {
    return d.location.path == "first.dola" && d.location.line == 1 &&
           d.location.column > 1;
  }));
  std::ostringstream output;
  diagnostics.print(output);
  const std::string text = output.str();
  const size_t second = text.find("second.dola:");
  const size_t first = text.find("first.dola:");
  ASSERT_NE(second, std::string::npos);
  ASSERT_NE(first, std::string::npos);
  EXPECT_LT(second, first);
}

TEST(SemanticAnalyzerTest, ChecksAggregateCollectionTypesAndControlFlow) {
  DiagnosticEngine diagnostics;
  auto result = analyzeSources(
      {{"features.dola",
        "module features; record Pair { left: Int, right: Int, } "
        "enum Choice { Empty, Value(Int), } "
        "fn unwrap(value: Option[Int]) -> Int { match value { None => 0, "
        "Some(item) => item, } } "
        "fn main() -> Int { let pair = Pair { left: 1, right: 2 }; "
        "let changed = pair with { right: 3 }; let tuple = (changed.left, "
        "changed.right); var values = [tuple.0]; values = "
        "values.push(tuple.1); "
        "var total = 0; for value in values { if value == 1 { continue; }; "
        "total = total + value; }; while total < 4 { total = total + 1; }; "
        "unwrap(values.get(0)) - 1 }"}},
      diagnostics, true);
  ASSERT_TRUE(result);
  EXPECT_FALSE(diagnostics.hasErrors());
  if (!result)
    return;
  EXPECT_FALSE(result->records.empty());
  EXPECT_FALSE(result->enums.empty());
  EXPECT_FALSE(result->patterns.empty());
  EXPECT_FALSE(result->loopBindings.empty());
}

TEST(SemanticAnalyzerTest, RejectsInvalidAggregateAndCollectionPrograms) {
  const std::vector<std::string> invalidSources = {
      R"(module recursive; record Node { next: Node, } fn main() {})",
      R"(module missing_arm; enum E { A, B, } fn main() -> Int { match E.A { E.A => 0, } })",
      R"(module duplicate_arm; fn main() -> Int { match true { true => 0, true => 1, false => 2, } })",
      R"(module map_key; fn main() { let invalid = Map[Bool, Int](); })",
      R"(module option_try; fn main() -> Result[Unit, String] { let value: Option[Int] = None; value?; Ok(()) })",
      R"(module outside_loop; fn main() { break; })",
      R"(module ambiguous; fn main() { let values = []; })",
  };
  for (size_t index = 0; index < invalidSources.size(); ++index) {
    DiagnosticEngine diagnostics;
    SCOPED_TRACE(index);
    EXPECT_FALSE(analyzeSources({{"invalid.dola", invalidSources[index]}},
                                diagnostics, true));
    EXPECT_TRUE(diagnostics.hasErrors());
  }
}

TEST(SemanticAnalyzerTest, ResolvesConcurrencyIoAndTransportTypes) {
  DiagnosticEngine diagnostics;
  auto result = analyzeSources({{"features.dola", R"(
module capabilities;
use std.io;
use std.time;
use std.transport;
record Message { text: String, }
fn worker(value: Int) -> Int { value + 1 }
fn main() {
  let task: Task[Int] = spawn worker(41);
  let joined: Result[Int, TaskError] = task.join();
  let (sender, receiver) = channel[Message]();
  let sent: Result[Unit, ChannelError] = sender.send(Message { text: "hi" });
  let received: Option[Message] = receiver.receive();
  sender.close();
  let line: Result[String, IoError] = io.read_line();
  let now = time.unix_seconds();
  time.sleep_ms(0);
  let parsed: Result[Int, ParseIntError] = Int.parse("42");
  let split: Option[(String, String)] = "a:b".split_once(":");
  let listener: Result[Listener[Message, Message], TransportError] =
      transport.listen[Message, Message]("127.0.0.1:0");
}
)"}},
                               diagnostics, true);
  std::ostringstream errors;
  diagnostics.print(errors);
  ASSERT_TRUE(result) << errors.str();
  EXPECT_FALSE(diagnostics.hasErrors());
}

TEST(SemanticAnalyzerTest, RejectsInvalidConcurrencyOperations) {
  const std::vector<std::string> invalidSources = {
      R"(module invalid; fn worker(value: Int) {} fn main() { let task = spawn { worker(1); }; })",
      R"(module invalid; fn main() { let pair = channel[Int, String](); })",
      R"(module invalid; fn main() { let (sender, receiver) = channel[Int](); sender.send("bad"); })",
      R"(module invalid; fn main() { let task: Task[Int] = 1; })",
      R"(module invalid; use std.transport; fn main() { let value = transport.listen[Int]("127.0.0.1:0"); })",
  };
  for (std::size_t index = 0; index < invalidSources.size(); ++index) {
    DiagnosticEngine diagnostics;
    SCOPED_TRACE(index);
    EXPECT_FALSE(analyzeSources({{"invalid.dola", invalidSources[index]}},
                                diagnostics, true));
    EXPECT_TRUE(diagnostics.hasErrors());
  }
}

} // namespace
} // namespace dola
