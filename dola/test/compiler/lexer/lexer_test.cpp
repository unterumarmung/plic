#include "dola_lexer.h"

#include "antlr4-runtime.h"
#include "gtest/gtest.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {

class ErrorCounter final : public antlr4::BaseErrorListener {
public:
  // ANTLR fixes this callback signature; strong types cannot be introduced at
  // the foreign virtual interface boundary.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void syntaxError(antlr4::Recognizer*, antlr4::Token*, std::size_t lineValue,
                   std::size_t columnValue, const std::string&,
                   std::exception_ptr) override {
    ++count;
    line = lineValue;
    column = columnValue;
  }

  std::size_t count = 0;
  std::size_t line = 0;
  std::size_t column = 0;
};

TEST(LexerTest, TokenizesKeywordsLiteralsCommentsAndLocations) {
  antlr4::ANTLRInputStream input(
      "// heading\nmodule sample; /* gap */ while \"a\\n\" 42 1.5");
  dola_lexer lexer(&input);
  antlr4::CommonTokenStream stream(&lexer);
  stream.fill();
  const std::vector<antlr4::Token*>& tokens = stream.getTokens();

  ASSERT_EQ(tokens.size(), 8U);
  EXPECT_EQ(tokens[0]->getType(), dola_lexer::MODULE);
  EXPECT_EQ(tokens[0]->getLine(), 2U);
  EXPECT_EQ(tokens[0]->getCharPositionInLine(), 0U);
  EXPECT_EQ(tokens[3]->getType(), dola_lexer::WHILE);
  EXPECT_EQ(tokens[3]->getText(), "while");
  EXPECT_EQ(tokens[4]->getType(), dola_lexer::STRING_LITERAL);
  EXPECT_EQ(tokens[4]->getText(), "\"a\\n\"");
  EXPECT_EQ(tokens[5]->getType(), dola_lexer::INT_LITERAL);
  EXPECT_EQ(tokens[6]->getType(), dola_lexer::FLOAT_LITERAL);
  EXPECT_EQ(tokens.back()->getType(), antlr4::Token::EOF);
}

TEST(LexerTest, ReportsMalformedStringsAtTheOpeningToken) {
  antlr4::ANTLRInputStream input("\"unterminated\n");
  dola_lexer lexer(&input);
  ErrorCounter errors;
  lexer.removeErrorListeners();
  lexer.addErrorListener(&errors);
  antlr4::CommonTokenStream stream(&lexer);
  stream.fill();

  EXPECT_EQ(errors.count, 1U);
  EXPECT_EQ(errors.line, 1U);
  EXPECT_EQ(errors.column, 0U);
}

TEST(LexerTest, ReportsIllegalCharactersAtTheirExactLocation) {
  antlr4::ANTLRInputStream input("\n  @");
  dola_lexer lexer(&input);
  ErrorCounter errors;
  lexer.removeErrorListeners();
  lexer.addErrorListener(&errors);
  antlr4::CommonTokenStream stream(&lexer);
  stream.fill();

  EXPECT_EQ(errors.count, 1U);
  EXPECT_EQ(errors.line, 2U);
  EXPECT_EQ(errors.column, 2U);
}

TEST(LexerTest, TokenizesLoopKeywordAtItsExactLocation) {
  antlr4::ANTLRInputStream input("  loop");
  dola_lexer lexer(&input);
  antlr4::CommonTokenStream stream(&lexer);
  stream.fill();
  const auto& tokens = stream.getTokens();
  ASSERT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens.front()->getType(), dola_lexer::LOOP);
  EXPECT_EQ(tokens.front()->getLine(), 1U);
  EXPECT_EQ(tokens.front()->getCharPositionInLine(), 2U);
}

TEST(LexerTest, DistinguishesEveryMultiCharacterOperator) {
  antlr4::ANTLRInputStream input("-> => .. || && == != <= >=");
  dola_lexer lexer(&input);
  antlr4::CommonTokenStream stream(&lexer);
  stream.fill();
  const std::vector<std::size_t> expected{
      dola_lexer::ARROW, dola_lexer::FAT_ARROW, dola_lexer::DOTDOT,
      dola_lexer::OR,    dola_lexer::AND,       dola_lexer::EQ,
      dola_lexer::NE,    dola_lexer::LE,        dola_lexer::GE,
      antlr4::Token::EOF};
  const auto& tokens = stream.getTokens();
  ASSERT_EQ(tokens.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index)
    EXPECT_EQ(tokens.at(index)->getType(), expected.at(index));
}

} // namespace
