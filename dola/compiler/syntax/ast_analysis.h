#ifndef DOLA_SYNTAX_AST_ANALYSIS_H
#define DOLA_SYNTAX_AST_ANALYSIS_H

#include "dola/ast.h"

namespace dola {

bool expressionContainsReturn(const Expr& expression);
bool blockContainsReturn(const Block& block);
bool expressionDefinitelyReturns(const Expr& expression);
bool blockDefinitelyReturns(const Block& block);
bool expressionContainsControlTransfer(const Expr& expression);
bool blockContainsControlTransfer(const Block& block);

} // namespace dola

#endif
