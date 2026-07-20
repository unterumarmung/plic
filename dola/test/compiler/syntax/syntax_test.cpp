#include "dola/ast.h"
#include "dola/ast_analysis.h"
#include "dola/diagnostic.h"
#include "dola/parser.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace dola {
namespace {

TEST(ParserTest, PreservesPrecedenceAndStableIds) {
  DiagnosticEngine diagnostics;
  auto module = parseSource(
      "precedence.dola",
      "module precedence; fn main() -> Int { 1 + 2 * 3 - 4 }", diagnostics);
  ASSERT_TRUE(module);
  if (!module)
    return;
  ASSERT_FALSE(diagnostics.hasErrors());
  const std::string printed = dumpAst(*module);
  EXPECT_NE(printed.find("        (binary -\n          (binary +\n"),
            std::string::npos);
  const Expr& root = *module->functions.front().body->tail;
  const auto& subtraction = std::get<BinaryExpr>(root.value);
  EXPECT_NE(root.id, subtraction.left->id);
  EXPECT_NE(subtraction.left->id, subtraction.right->id);
}

TEST(ParserTest, StableIdsRepeatForTheSamePathAndSource) {
  DiagnosticEngine firstDiagnostics;
  DiagnosticEngine secondDiagnostics;
  auto first =
      parseSource("stable.dola", "module stable; fn main() -> Int { 40 + 2 }",
                  firstDiagnostics);
  auto second =
      parseSource("stable.dola", "module stable; fn main() -> Int { 40 + 2 }",
                  secondDiagnostics);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  if (!first || !second)
    return;
  EXPECT_EQ(first->functions.front().id, second->functions.front().id);
  EXPECT_EQ(first->functions.front().body->tail->id,
            second->functions.front().body->tail->id);
}

TEST(AstPrinterTest, BalancesNestedNodesAndEndsWithNewline) {
  DiagnosticEngine diagnostics;
  auto module = parseSource(
      "printer.dola",
      "module printer; record R { value: Int, } enum E { Empty, Value(Int), } "
      "fn main() -> Int { let item = R { value: 1 }; match E.Value(item.value) "
      "{ E.Empty => 0, E.Value(value) => value, } }",
      diagnostics);
  ASSERT_TRUE(module);
  if (!module)
    return;
  const std::string printed = dumpAst(*module);
  EXPECT_TRUE(printed.ends_with('\n'));
  EXPECT_EQ(std::ranges::count(printed, '('), std::ranges::count(printed, ')'));
  EXPECT_LT(printed.find("(record R"), printed.find("(enum E"));
  EXPECT_LT(printed.find("(enum E"), printed.find("(fn main"));
}

TEST(ParserTest, NormalizesLiteralEscapesAndBoundaries) {
  DiagnosticEngine diagnostics;
  auto module = parseSource(
      "literals.dola",
      "module literals; fn minimum() -> Int { -9223372036854775808 } "
      "fn text() -> String { \"a\\n\\\"b\" }",
      diagnostics);
  ASSERT_TRUE(module);
  if (!module)
    return;
  EXPECT_FALSE(diagnostics.hasErrors());
  EXPECT_NE(dumpAst(*module).find("\"a\\n\\\"b\""), std::string::npos);
}

TEST(ParserTest, ReportsOutOfRangeInteger) {
  DiagnosticEngine diagnostics;
  EXPECT_FALSE(
      parseSource("overflow.dola",
                  "module overflow; fn value() -> Int { 9223372036854775808 }",
                  diagnostics));
  EXPECT_TRUE(diagnostics.hasErrors());
}

TEST(ParserTest, RecoversAndReportsMultipleSyntaxErrors) {
  DiagnosticEngine diagnostics;
  EXPECT_FALSE(parseSource(
      "recovery.dola",
      "module recovery; fn first( { return; } fn second() { let = 1; }",
      diagnostics));
  EXPECT_GE(diagnostics.diagnostics().size(), 2U);
}

TEST(AstAnalysisTest, DistinguishesContainedAndDefiniteReturns) {
  DiagnosticEngine diagnostics;
  auto module = parseSource(
      "returns.dola",
      "module returns; fn maybe() -> Int { if true { return 1; }; 0 } "
      "fn always() -> Int { if true { return 1; } else { return 2; }; }",
      diagnostics);
  ASSERT_TRUE(module);
  if (!module)
    return;
  ASSERT_FALSE(diagnostics.hasErrors());
  EXPECT_TRUE(blockContainsReturn(*module->functions[0].body));
  EXPECT_FALSE(blockDefinitelyReturns(*module->functions[0].body));
  EXPECT_TRUE(blockDefinitelyReturns(*module->functions[1].body));
}

TEST(ParserTest,
     ParsesAggregateCollectionAndControlSyntaxWithStablePatternIds) {
  DiagnosticEngine diagnostics;
  auto module = parseSource(
      "features.dola",
      "module features; record Pair { left: Int, right: Int, } "
      "enum Value { Empty, Number(Int), } "
      "fn main() -> Int { let pair = Pair { left: 1, right: 2 }; "
      "let tuple = (pair.left, pair.right); let values = [tuple.0, tuple.1]; "
      "for value in values { if value == 0 { continue; }; }; "
      "match Value.Number(2) { Value.Empty => 0, Value.Number(number) => "
      "number, } }",
      diagnostics);
  ASSERT_TRUE(module);
  ASSERT_FALSE(diagnostics.hasErrors());
  if (!module)
    return;
  EXPECT_EQ(module->records.size(), 1U);
  EXPECT_EQ(module->enums.size(), 1U);
  const std::string printed = dumpAst(*module);
  EXPECT_NE(printed.find("(record Pair"), std::string::npos);
  EXPECT_NE(printed.find("(enum Value"), std::string::npos);
  EXPECT_NE(printed.find("(for value"), std::string::npos);
  EXPECT_NE(printed.find("(match"), std::string::npos);
  const auto& match =
      std::get<MatchExpr>(module->functions.front().body->tail->value);
  ASSERT_EQ(match.arms.size(), 2U);
  EXPECT_NE(match.arms[0].pattern->id, match.arms[1].pattern->id);
}

TEST(ParserTest, ParsesExplicitNestedLoopsWithStableExpressionIds) {
  DiagnosticEngine firstDiagnostics;
  DiagnosticEngine secondDiagnostics;
  const std::string source =
      "module loops; fn main() { var count = 0; loop { loop { count = count "
      "+ 1; break; }; if count == 2 { break; }; continue; }; }";
  auto first = parseSource("loops.dola", source, firstDiagnostics);
  auto second = parseSource("loops.dola", source, secondDiagnostics);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_FALSE(firstDiagnostics.hasErrors());
  ASSERT_FALSE(secondDiagnostics.hasErrors());
  if (!first || !second)
    return;

  const std::string printed = dumpAst(*first);
  EXPECT_NE(printed.find("(loop"), printed.rfind("(loop"));
  const auto& outer = *std::get<ExpressionStmt>(
                           first->functions.front().body->statements.at(1).value)
                           .value;
  const auto& repeated = *std::get<ExpressionStmt>(
                              second->functions.front().body->statements.at(1)
                                  .value)
                              .value;
  ASSERT_TRUE(std::holds_alternative<LoopExpr>(outer.value));
  ASSERT_TRUE(std::holds_alternative<LoopExpr>(repeated.value));
  EXPECT_EQ(outer.id, repeated.id);
}

} // namespace
} // namespace dola
