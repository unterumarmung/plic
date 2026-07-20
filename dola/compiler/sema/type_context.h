#ifndef DOLA_SEMA_TYPE_CONTEXT_H
#define DOLA_SEMA_TYPE_CONTEXT_H

#include "dola/sema.h"

namespace dola::sema::detail {

class TypeContext {
public:
  explicit TypeContext(TypeTable& table) : table_(table) {}

  void initializeBuiltins();
  TypeId intern(TypeKind kind, std::vector<TypeId> arguments = {},
                std::optional<TypeDeclarationRef> declaration = std::nullopt);

private:
  TypeTable& table_;
};

} // namespace dola::sema::detail

#endif
