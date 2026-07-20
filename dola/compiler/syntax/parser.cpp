#include "dola/parser.h"

#include "antlr4-runtime.h"
#include "dola_lexer.h"
#include "dola_parser.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

namespace dola {
namespace {

SourceLocation locationOf(const std::filesystem::path& path,
                          antlr4::Token* token) {
  if (!token)
    return {.path = path};
  return {.path = path,
          .line = static_cast<uint32_t>(token->getLine()),
          .column = static_cast<uint32_t>(token->getCharPositionInLine() + 1),
          .length = static_cast<uint32_t>(
              std::max<size_t>(1, token->getText().size()))};
}

class SyntaxErrorListener final : public antlr4::BaseErrorListener {
public:
  SyntaxErrorListener(std::filesystem::path path, DiagnosticEngine& diagnostics)
      : path_(std::move(path)), diagnostics_(diagnostics) {}

  void syntaxError(antlr4::Recognizer*, antlr4::Token* offendingSymbol,
                   size_t line, size_t charPositionInLine,
                   const std::string& message, std::exception_ptr) override {
    diagnostics_.error(
        {path_, static_cast<uint32_t>(line),
         static_cast<uint32_t>(charPositionInLine + 1),
         static_cast<uint32_t>(
             offendingSymbol
                 ? std::max<size_t>(1, offendingSymbol->getText().size())
                 : 1)},
        message);
  }

private:
  std::filesystem::path path_;
  DiagnosticEngine& diagnostics_;
};

class AstBuilder {
public:
  AstBuilder(std::filesystem::path path, DiagnosticEngine& diagnostics)
      : path_(std::move(path)), diagnostics_(diagnostics) {}

  Module build(dola_parser::SourceFileContext* context) {
    Module module;
    module.path = path_;
    module.location = loc(context->moduleDecl());
    module.name = name(context->moduleDecl()->qualifiedName());
    for (auto* use : context->useDecl())
      module.imports.push_back({loc(use), name(use->qualifiedName())});
    for (auto* declaration : context->topLevelDecl()) {
      if (declaration->functionDecl()) {
        module.declarations.push_back({FunctionIndex{module.functions.size()}});
        module.functions.push_back(fn(declaration->functionDecl()));
      } else if (declaration->recordDecl()) {
        module.declarations.push_back({RecordIndex{module.records.size()}});
        module.records.push_back(record(declaration->recordDecl()));
      } else {
        module.declarations.push_back({EnumIndex{module.enums.size()}});
        module.enums.push_back(enumeration(declaration->enumDecl()));
      }
    }
    return module;
  }

private:
  SourceLocation loc(antlr4::ParserRuleContext* context) const {
    return locationOf(path_, context ? context->getStart() : nullptr);
  }

  std::vector<std::string> name(dola_parser::QualifiedNameContext* context) {
    std::vector<std::string> result;
    for (auto* identifier : context->IDENT())
      result.push_back(identifier->getText());
    return result;
  }

  TypeRef unit(antlr4::ParserRuleContext* context) {
    return {loc(context), PrimitiveTypeRef{PrimitiveType::Unit}};
  }

  TypeRef type(dola_parser::TypeRefContext* context) {
    if (auto* primitive = context->primitiveType()) {
      PrimitiveType kind = PrimitiveType::String;
      if (primitive->UNIT())
        kind = PrimitiveType::Unit;
      else if (primitive->BOOL())
        kind = PrimitiveType::Bool;
      else if (primitive->INT())
        kind = PrimitiveType::Int;
      else if (primitive->FLOAT())
        kind = PrimitiveType::Float;
      return {loc(context), PrimitiveTypeRef{kind}};
    }
    if (context->qualifiedName())
      return {loc(context), NamedTypeRef{name(context->qualifiedName())}};
    std::vector<TypeRefPtr> arguments;
    for (auto* argument : context->typeRef())
      arguments.push_back(std::make_unique<TypeRef>(type(argument)));
    if (context->LPAREN())
      return {loc(context), TupleTypeRef{std::move(arguments)}};
    BuiltinType kind = BuiltinType::Map;
    if (context->OPTION())
      kind = BuiltinType::Option;
    else if (context->RESULT())
      kind = BuiltinType::Result;
    else if (context->LIST())
      kind = BuiltinType::List;
    else if (context->SENDER())
      kind = BuiltinType::Sender;
    else if (context->RECEIVER())
      kind = BuiltinType::Receiver;
    else if (context->TASK())
      kind = BuiltinType::Task;
    else if (context->LISTENER())
      kind = BuiltinType::Listener;
    else if (context->CONNECTION())
      kind = BuiltinType::Connection;
    return {loc(context), BuiltinTypeRef{kind, std::move(arguments)}};
  }

  FunctionDecl fn(dola_parser::FunctionDeclContext* context) {
    auto result = FunctionDecl{stableId<DeclId>("decl", nextDecl_++),
                               loc(context),
                               context->PUB() != nullptr,
                               context->IDENT()->getText(),
                               {},
                               context->typeRef() ? type(context->typeRef())
                                                  : unit(context),
                               block(context->block())};
    if (auto* parameters = context->parameterList())
      for (auto* parameter : parameters->parameter())
        result.parameters.push_back({loc(parameter),
                                     parameter->IDENT()->getText(),
                                     type(parameter->typeRef())});
    return result;
  }

  RecordDecl record(dola_parser::RecordDeclContext* context) {
    RecordDecl result{stableId<DeclId>("decl", nextDecl_++),
                      loc(context),
                      context->PUB() != nullptr,
                      context->IDENT()->getText(),
                      {}};
    for (auto* field : context->recordFieldDecl())
      result.fields.push_back(
          {loc(field), field->IDENT()->getText(), type(field->typeRef())});
    return result;
  }

  EnumDecl enumeration(dola_parser::EnumDeclContext* context) {
    EnumDecl result{stableId<DeclId>("decl", nextDecl_++),
                    loc(context),
                    context->PUB() != nullptr,
                    context->IDENT()->getText(),
                    {}};
    for (auto* variant : context->enumVariantDecl()) {
      EnumVariantDecl value{loc(variant), variant->IDENT()->getText(), {}};
      if (auto* types = variant->typeList())
        for (auto* payload : types->typeRef())
          value.payloads.push_back(type(payload));
      result.variants.push_back(std::move(value));
    }
    return result;
  }

  std::unique_ptr<Block> block(dola_parser::BlockContext* context) {
    auto result = std::make_unique<Block>();
    result->location = loc(context);
    for (auto* statement : context->statement())
      result->statements.push_back(stmt(statement));
    if (context->expression())
      result->tail = expr(context->expression());
    return result;
  }

  BindingTarget bindingTarget(dola_parser::BindingTargetContext* context) {
    if (context->IDENT().size() == 1)
      return BindingName{context->IDENT().front()->getText()};
    TupleBinding tuple;
    for (auto* identifier : context->IDENT())
      tuple.names.push_back(identifier->getText());
    return tuple;
  }

  Statement stmt(dola_parser::StatementContext* context) {
    if (auto* binding =
            dynamic_cast<dola_parser::BindingStatementContext*>(context)) {
      BindingStmt value{binding->VAR() != nullptr,
                        bindingTarget(binding->bindingTarget()), std::nullopt,
                        expr(binding->expression())};
      if (binding->typeRef())
        value.annotation = type(binding->typeRef());
      return {loc(context), std::move(value)};
    }
    if (auto* assignment =
            dynamic_cast<dola_parser::AssignmentStatementContext*>(context))
      return {loc(context), AssignmentStmt{assignment->IDENT()->getText(),
                                           expr(assignment->expression())}};
    if (auto* expression =
            dynamic_cast<dola_parser::ExpressionStatementContext*>(context))
      return {loc(context), ExpressionStmt{expr(expression->expression())}};
    if (auto* loop = dynamic_cast<dola_parser::WhileStatementContext*>(context))
      return {loc(context), ExpressionStmt{whileExpr(loop->whileExpression())}};
    if (auto* loop = dynamic_cast<dola_parser::ForStatementContext*>(context))
      return {loc(context), ExpressionStmt{forExpr(loop->forExpression())}};
    auto* loop = dynamic_cast<dola_parser::LoopStatementContext*>(context);
    assert(loop != nullptr);
    return {loc(context), ExpressionStmt{loopExpr(loop->loopExpression())}};
  }

  template <typename Context, typename Child, typename Parse>
  ExprPtr fold(Context* context, const std::vector<Child*>& operands,
               Parse parse) {
    auto result = parse(operands.front());
    for (size_t i = 1; i < operands.size(); ++i) {
      const std::string op = context->children[i * 2 - 1]->getText();
      result =
          make(context, BinaryExpr{op, std::move(result), parse(operands[i])});
    }
    return result;
  }

  template <typename Value>
  ExprPtr make(antlr4::ParserRuleContext* context, Value value) {
    return std::make_unique<Expr>(Expr{stableId<ExprId>("expr", nextExpr_++),
                                       loc(context), std::move(value)});
  }

  ExprPtr integer(antlr4::ParserRuleContext* context, std::string_view text) {
    int64_t value = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
      diagnostics_.error(loc(context),
                         "integer literal is outside the range of `Int`");
      value = 0;
    }
    return make(context, LiteralExpr{value});
  }

  ExprPtr expr(dola_parser::ExpressionContext* context) {
    if (context->controlExpression()) {
      auto* control = context->controlExpression();
      ControlKind kind = ControlKind::Continue;
      if (control->RETURN())
        kind = ControlKind::Return;
      else if (control->BREAK())
        kind = ControlKind::Break;
      ExprPtr value;
      if (control->expression())
        value = expr(control->expression());
      return make(control, ControlExpr{kind, std::move(value)});
    }
    return range(context->range());
  }
  ExprPtr range(dola_parser::RangeContext* context) {
    if (context->logicalOr().size() == 1)
      return logicalOr(context->logicalOr().front());
    return make(context, RangeExpr{logicalOr(context->logicalOr(0)),
                                   logicalOr(context->logicalOr(1))});
  }
  ExprPtr logicalOr(dola_parser::LogicalOrContext* context) {
    return fold(context, context->logicalAnd(),
                [&](auto* child) { return logicalAnd(child); });
  }
  ExprPtr logicalAnd(dola_parser::LogicalAndContext* context) {
    return fold(context, context->equality(),
                [&](auto* child) { return equality(child); });
  }
  ExprPtr equality(dola_parser::EqualityContext* context) {
    return fold(context, context->comparison(),
                [&](auto* child) { return comparison(child); });
  }
  ExprPtr comparison(dola_parser::ComparisonContext* context) {
    return fold(context, context->additive(),
                [&](auto* child) { return additive(child); });
  }
  ExprPtr additive(dola_parser::AdditiveContext* context) {
    return fold(context, context->multiplicative(),
                [&](auto* child) { return multiplicative(child); });
  }
  ExprPtr multiplicative(dola_parser::MultiplicativeContext* context) {
    return fold(context, context->unary(),
                [&](auto* child) { return unary(child); });
  }
  ExprPtr unary(dola_parser::UnaryContext* context) {
    if (context->SPAWN())
      return make(context, SpawnExpr{postfix(context->postfix())});
    if (context->update())
      return update(context->update());
    if (context->children.front()->getText() == "-" &&
        context->unary()->update() && context->unary()->update()->postfix() &&
        context->unary()->update()->postfix()->postfixSuffix().empty() &&
        context->unary()->update()->postfix()->primary()->integerLiteral())
      return integer(context, context->getText());
    return make(context, UnaryExpr{context->children.front()->getText(),
                                   unary(context->unary())});
  }
  ExprPtr update(dola_parser::UpdateContext* context) {
    auto result = postfix(context->postfix());
    if (context->recordBody())
      result =
          make(context, RecordUpdateExpr{std::move(result),
                                         fieldValues(context->recordBody())});
    return result;
  }
  ExprPtr postfix(dola_parser::PostfixContext* context) {
    auto result = primary(context->primary());
    for (auto* suffix : context->postfixSuffix()) {
      if (suffix->LPAREN()) {
        std::vector<TypeRef> typeArguments;
        if (auto* types = suffix->typeArguments())
          for (auto* argument : types->typeList()->typeRef())
            typeArguments.push_back(type(argument));
        std::vector<ExprPtr> arguments;
        if (auto* list = suffix->argumentList())
          for (auto* argument : list->expression())
            arguments.push_back(expr(argument));
        result =
            make(suffix, CallExpr{std::move(result), std::move(typeArguments),
                                  std::move(arguments)});
      } else if (suffix->QUESTION()) {
        result = make(suffix, TryExpr{std::move(result)});
      } else if (suffix->IDENT()) {
        result = make(
            suffix, MemberExpr{std::move(result), suffix->IDENT()->getText()});
      } else {
        std::size_t index = 0;
        const std::string text = suffix->INT_LITERAL()->getText();
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
          diagnostics_.error(
              locationOf(path_, suffix->INT_LITERAL()->getSymbol()),
              "tuple field index is outside the supported range");
        result = make(suffix,
                      MemberExpr{std::move(result), TupleElementIndex{index}});
      }
    }
    return result;
  }

  std::string decodeString(std::string text) {
    std::string result;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
      if (text[i] != '\\') {
        result.push_back(text[i]);
        continue;
      }
      switch (text[++i]) {
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case '"':
        result.push_back('"');
        break;
      default:
        result.push_back(text[i]);
        break;
      }
    }
    return result;
  }

  std::vector<RecordFieldValue>
  fieldValues(dola_parser::RecordBodyContext* context) {
    std::vector<RecordFieldValue> result;
    for (auto* field : context->recordFieldValue())
      result.push_back(
          {loc(field), field->IDENT()->getText(), expr(field->expression())});
    return result;
  }

  ExprPtr pathExpression(antlr4::ParserRuleContext* context,
                         const std::vector<std::string>& components) {
    auto result = make(context, NameExpr{{components.front()}});
    for (size_t i = 1; i < components.size(); ++i)
      result = make(context, MemberExpr{std::move(result), components[i]});
    return result;
  }

  ExprPtr primary(dola_parser::PrimaryContext* context) {
    if (context->integerLiteral())
      return integer(context, context->integerLiteral()->getText());
    if (context->FLOAT_LITERAL()) {
      errno = 0;
      double value =
          std::strtod(context->FLOAT_LITERAL()->getText().c_str(), nullptr);
      if (errno == ERANGE || !std::isfinite(value)) {
        diagnostics_.error(
            loc(context),
            "floating-point literal is outside the range of `Float`");
        value = 0.0;
      }
      return make(context, LiteralExpr{value});
    }
    if (context->TRUE())
      return make(context, LiteralExpr{true});
    if (context->FALSE())
      return make(context, LiteralExpr{false});
    if (context->STRING_LITERAL())
      return make(context, LiteralExpr{decodeString(
                               context->STRING_LITERAL()->getText())});
    if (auto* record = context->recordExpression())
      return make(record, RecordExpr{name(record->qualifiedName()),
                                     fieldValues(record->recordBody())});
    if (auto* constructor = context->genericConstructor()) {
      BuiltinType kind =
          constructor->LIST() ? BuiltinType::List : BuiltinType::Map;
      std::vector<TypeRef> arguments;
      for (auto* argument : constructor->typeList()->typeRef())
        arguments.push_back(type(argument));
      return make(constructor,
                  GenericConstructorExpr{kind, std::move(arguments)});
    }
    if (context->CHANNEL())
      return make(context, NameExpr{{"channel"}});
    if (context->INT() && context->IDENT())
      return pathExpression(context, {"Int", context->IDENT()->getText()});
    if (context->qualifiedName())
      return pathExpression(context, name(context->qualifiedName()));
    if (auto* tuple = context->tupleExpression()) {
      std::vector<ExprPtr> elements;
      for (auto* element : tuple->expression())
        elements.push_back(expr(element));
      return make(tuple, TupleExpr{std::move(elements)});
    }
    if (auto* list = context->listExpression()) {
      std::vector<ExprPtr> elements;
      if (auto* arguments = list->argumentList())
        for (auto* element : arguments->expression())
          elements.push_back(expr(element));
      return make(list, ListExpr{std::move(elements)});
    }
    if (context->ifExpression())
      return ifExpr(context->ifExpression());
    if (context->matchExpression())
      return matchExpr(context->matchExpression());
    if (context->whileExpression())
      return whileExpr(context->whileExpression());
    if (context->forExpression())
      return forExpr(context->forExpression());
    if (context->loopExpression())
      return loopExpr(context->loopExpression());
    if (context->block())
      return make(context, BlockExpr{block(context->block())});
    if (context->expression())
      return expr(context->expression());
    return make(context, LiteralExpr{std::monostate{}});
  }

  ExprPtr ifExpr(dola_parser::IfExpressionContext* context) {
    IfExpr result{expr(context->expression()), block(context->block(0)),
                  std::monostate{}};
    if (context->ifExpression())
      result.elseBranch = ifExpr(context->ifExpression());
    else if (context->block().size() > 1)
      result.elseBranch = block(context->block(1));
    return make(context, std::move(result));
  }

  PatternPtr pattern(dola_parser::PatternContext* context) {
    auto result = std::make_unique<Pattern>(
        Pattern{stableId<PatternId>("pattern", nextPattern_++), loc(context),
                WildcardPattern{}});
    if (context->TRUE())
      result->value = LiteralPattern{true};
    else if (context->FALSE())
      result->value = LiteralPattern{false};
    else if (context->integerLiteral()) {
      int64_t value = 0;
      const std::string text = context->integerLiteral()->getText();
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{})
        diagnostics_.error(loc(context),
                           "integer pattern is outside the range of `Int`");
      result->value = LiteralPattern{value};
    } else if (context->STRING_LITERAL()) {
      result->value =
          LiteralPattern{decodeString(context->STRING_LITERAL()->getText())};
    } else {
      auto components = name(context->qualifiedName());
      if (components.size() == 1 && components.front() == "_") {
        result->value = WildcardPattern{};
      } else if (components.size() == 1 && !context->LPAREN() &&
                 !components.front().empty() &&
                 (components.front().front() == '_' ||
                  (components.front().front() >= 'a' &&
                   components.front().front() <= 'z'))) {
        result->value = BindingPattern{components.front()};
      } else {
        ConstructorPattern constructor{std::move(components), {}};
        if (auto* list = context->patternList())
          for (auto* argument : list->pattern())
            constructor.arguments.push_back(pattern(argument));
        result->value = std::move(constructor);
      }
    }
    return result;
  }

  ExprPtr matchExpr(dola_parser::MatchExpressionContext* context) {
    MatchExpr result{expr(context->expression()), {}};
    for (auto* arm : context->matchArm())
      result.arms.push_back(
          {loc(arm), pattern(arm->pattern()), expr(arm->expression())});
    return make(context, std::move(result));
  }

  ExprPtr whileExpr(dola_parser::WhileExpressionContext* context) {
    return make(context, WhileExpr{expr(context->expression()),
                                   block(context->block())});
  }

  ExprPtr forExpr(dola_parser::ForExpressionContext* context) {
    return make(context,
                ForExpr{context->IDENT()->getText(),
                        expr(context->expression()), block(context->block())});
  }

  ExprPtr loopExpr(dola_parser::LoopExpressionContext* context) {
    return make(context, LoopExpr{block(context->block())});
  }

  std::filesystem::path path_;
  DiagnosticEngine& diagnostics_;
  uint64_t nextExpr_ = 1;
  uint64_t nextDecl_ = 1;
  uint64_t nextPattern_ = 1;

  template <typename Id>
  [[nodiscard]]
  Id stableId(std::string_view kind, uint64_t ordinal) const {
    uint64_t hash = 1469598103934665603ULL;
    auto add = [&](unsigned char byte) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    };
    for (unsigned char byte : path_.generic_string())
      add(byte);
    add(0xff);
    for (unsigned char byte : kind)
      add(byte);
    for (unsigned shift = 0; shift != 64; shift += 8)
      add(static_cast<unsigned char>(ordinal >> shift));
    return Id{hash};
  }
};

} // namespace

std::optional<Module> parseSource(const std::filesystem::path& path,
                                  const std::string& source,
                                  DiagnosticEngine& diagnostics) {
  diagnostics.registerSource(path, source);
  antlr4::ANTLRInputStream input(source);
  dola_lexer lexer(&input);
  SyntaxErrorListener listener(path, diagnostics);
  lexer.removeErrorListeners();
  lexer.addErrorListener(&listener);
  antlr4::CommonTokenStream tokens(&lexer);
  tokens.fill();
  tokens.seek(0);
  dola_parser parser(&tokens);
  parser.removeErrorListeners();
  parser.addErrorListener(&listener);
  auto* tree = parser.sourceFile();
  if (diagnostics.hasErrors())
    return std::nullopt;
  auto module = AstBuilder(path, diagnostics).build(tree);
  if (diagnostics.hasErrors())
    return std::nullopt;
  return module;
}

} // namespace dola
