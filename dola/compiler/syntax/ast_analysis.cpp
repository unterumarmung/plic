#include "dola/ast_analysis.h"

#include "dola/overloaded.h"

namespace dola {
namespace {

bool blockHasReturn(const Block& block, bool definite);
bool blockHasControl(const Block& block);

bool expressionHasControl(const Expr& expression) {
  auto expressionsHaveControl = [](const auto& values) {
    for (const auto& value : values)
      if (expressionHasControl(*value))
        return true;
    return false;
  };
  return std::visit(
      Overloaded{
          [](const LiteralExpr&) { return false; },
          [](const NameExpr&) { return false; },
          [&](const CallExpr& value) {
            return expressionHasControl(*value.callee) ||
                   expressionsHaveControl(value.arguments);
          },
          [&](const SpawnExpr& value) {
            return expressionHasControl(*value.call);
          },
          [&](const MemberExpr& value) {
            return expressionHasControl(*value.base);
          },
          [&](const UnaryExpr& value) {
            return expressionHasControl(*value.operand);
          },
          [&](const BinaryExpr& value) {
            return expressionHasControl(*value.left) ||
                   expressionHasControl(*value.right);
          },
          [&](const IfExpr& value) {
            if (expressionHasControl(*value.condition) ||
                blockHasControl(*value.thenBlock))
              return true;
            return std::visit(
                Overloaded{[](const std::monostate&) { return false; },
                           [&](const std::unique_ptr<Block>& branch) {
                             return blockHasControl(*branch);
                           },
                           [&](const ExprPtr& branch) {
                             return expressionHasControl(*branch);
                           }},
                value.elseBranch);
          },
          [&](const BlockExpr& value) { return blockHasControl(*value.block); },
          [&](const TupleExpr& value) {
            return expressionsHaveControl(value.elements);
          },
          [&](const ListExpr& value) {
            return expressionsHaveControl(value.elements);
          },
          [](const GenericConstructorExpr&) { return false; },
          [&](const RecordExpr& value) {
            for (const auto& field : value.fields)
              if (expressionHasControl(*field.value))
                return true;
            return false;
          },
          [&](const RecordUpdateExpr& value) {
            if (expressionHasControl(*value.base))
              return true;
            for (const auto& field : value.fields)
              if (expressionHasControl(*field.value))
                return true;
            return false;
          },
          [&](const TryExpr& value) {
            return expressionHasControl(*value.value);
          },
          [&](const RangeExpr& value) {
            return expressionHasControl(*value.start) ||
                   expressionHasControl(*value.end);
          },
          [&](const MatchExpr& value) {
            if (expressionHasControl(*value.value))
              return true;
            for (const auto& arm : value.arms)
              if (expressionHasControl(*arm.value))
                return true;
            return false;
          },
          [&](const WhileExpr& value) {
            return expressionHasControl(*value.condition) ||
                   blockHasControl(*value.body);
          },
          [&](const ForExpr& value) {
            return expressionHasControl(*value.iterable) ||
                   blockHasControl(*value.body);
          },
          [&](const LoopExpr& value) { return blockHasControl(*value.body); },
          [](const ControlExpr&) { return true; }},
      expression.value);
}

bool blockHasControl(const Block& block) {
  for (const auto& statement : block.statements) {
    bool found =
        std::visit(Overloaded{[&](const BindingStmt& value) {
                                return expressionHasControl(*value.initializer);
                              },
                              [&](const AssignmentStmt& value) {
                                return expressionHasControl(*value.value);
                              },
                              [&](const ExpressionStmt& value) {
                                return expressionHasControl(*value.value);
                              }},
                   statement.value);
    if (found)
      return true;
  }
  return block.tail && expressionHasControl(*block.tail);
}

bool expressionHasReturn(const Expr& expression, bool definite) {
  return std::visit(
      Overloaded{
          [](const LiteralExpr&) { return false; },
          [](const NameExpr&) { return false; },
          [&](const CallExpr& value) {
            if (expressionHasReturn(*value.callee, definite))
              return true;
            for (const auto& argument : value.arguments)
              if (expressionHasReturn(*argument, definite))
                return true;
            return false;
          },
          [&](const SpawnExpr& value) {
            return expressionHasReturn(*value.call, definite);
          },
          [&](const MemberExpr& value) {
            return expressionHasReturn(*value.base, definite);
          },
          [&](const UnaryExpr& value) {
            return expressionHasReturn(*value.operand, definite);
          },
          [&](const BinaryExpr& value) {
            if (definite)
              return expressionHasReturn(*value.left, true);
            return expressionHasReturn(*value.left, false) ||
                   expressionHasReturn(*value.right, false);
          },
          [&](const IfExpr& value) {
            if (!definite) {
              if (expressionHasReturn(*value.condition, false) ||
                  blockHasReturn(*value.thenBlock, false))
                return true;
              return std::visit(
                  Overloaded{[](const std::monostate&) { return false; },
                             [&](const std::unique_ptr<Block>& branch) {
                               return blockHasReturn(*branch, false);
                             },
                             [&](const ExprPtr& branch) {
                               return expressionHasReturn(*branch, false);
                             }},
                  value.elseBranch);
            }
            if (expressionHasReturn(*value.condition, true))
              return true;
            const bool thenReturns = blockHasReturn(*value.thenBlock, true);
            return std::visit(
                Overloaded{[](const std::monostate&) { return false; },
                           [&](const std::unique_ptr<Block>& branch) {
                             return thenReturns &&
                                    blockHasReturn(*branch, true);
                           },
                           [&](const ExprPtr& branch) {
                             return thenReturns &&
                                    expressionHasReturn(*branch, true);
                           }},
                value.elseBranch);
          },
          [&](const BlockExpr& value) {
            return blockHasReturn(*value.block, definite);
          },
          [&](const TupleExpr& value) {
            for (const auto& element : value.elements)
              if (expressionHasReturn(*element, definite))
                return true;
            return false;
          },
          [&](const ListExpr& value) {
            for (const auto& element : value.elements)
              if (expressionHasReturn(*element, definite))
                return true;
            return false;
          },
          [](const GenericConstructorExpr&) { return false; },
          [&](const RecordExpr& value) {
            for (const auto& field : value.fields)
              if (expressionHasReturn(*field.value, definite))
                return true;
            return false;
          },
          [&](const RecordUpdateExpr& value) {
            if (expressionHasReturn(*value.base, definite))
              return true;
            for (const auto& field : value.fields)
              if (expressionHasReturn(*field.value, definite))
                return true;
            return false;
          },
          [&](const TryExpr& value) {
            return definite || expressionHasReturn(*value.value, false);
          },
          [&](const RangeExpr& value) {
            return expressionHasReturn(*value.start, definite) ||
                   expressionHasReturn(*value.end, definite);
          },
          [&](const MatchExpr& value) {
            if (expressionHasReturn(*value.value, definite))
              return true;
            if (!definite) {
              for (const auto& arm : value.arms)
                if (expressionHasReturn(*arm.value, false))
                  return true;
              return false;
            }
            if (value.arms.empty())
              return false;
            for (const auto& arm : value.arms)
              if (!expressionHasReturn(*arm.value, true))
                return false;
            return true;
          },
          [&](const WhileExpr& value) {
            return !definite && (expressionHasReturn(*value.condition, false) ||
                                 blockHasReturn(*value.body, false));
          },
          [&](const ForExpr& value) {
            return !definite && (expressionHasReturn(*value.iterable, false) ||
                                 blockHasReturn(*value.body, false));
          },
          [&](const LoopExpr& value) {
            return !definite && blockHasReturn(*value.body, false);
          },
          [&](const ControlExpr& value) {
            if (value.kind == ControlKind::Return)
              return true;
            return value.value && expressionHasReturn(*value.value, definite);
          }},
      expression.value);
}

bool blockHasReturn(const Block& block, bool definite) {
  for (const auto& statement : block.statements) {
    const bool returns = std::visit(
        Overloaded{[&](const BindingStmt& binding) {
                     return expressionHasReturn(*binding.initializer, definite);
                   },
                   [&](const AssignmentStmt& assignment) {
                     return expressionHasReturn(*assignment.value, definite);
                   },
                   [&](const ExpressionStmt& expression) {
                     return expressionHasReturn(*expression.value, definite);
                   }},
        statement.value);
    if (returns)
      return true;
  }
  return block.tail && expressionHasReturn(*block.tail, definite);
}

} // namespace

bool expressionContainsReturn(const Expr& expression) {
  return expressionHasReturn(expression, false);
}
bool blockContainsReturn(const Block& block) {
  return blockHasReturn(block, false);
}
bool expressionDefinitelyReturns(const Expr& expression) {
  return expressionHasReturn(expression, true);
}
bool blockDefinitelyReturns(const Block& block) {
  return blockHasReturn(block, true);
}
bool expressionContainsControlTransfer(const Expr& expression) {
  return expressionHasControl(expression);
}
bool blockContainsControlTransfer(const Block& block) {
  return blockHasControl(block);
}

} // namespace dola
