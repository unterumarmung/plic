parser grammar dola_parser;

options {
  tokenVocab = dola_lexer;
}

sourceFile: moduleDecl useDecl* topLevelDecl* EOF;
moduleDecl: MODULE qualifiedName SEMI;
useDecl: USE qualifiedName SEMI;
topLevelDecl: functionDecl | recordDecl | enumDecl;
functionDecl: PUB? FN IDENT LPAREN parameterList? RPAREN (ARROW typeRef)? block;
recordDecl: PUB? RECORD IDENT LBRACE recordFieldDecl* RBRACE;
recordFieldDecl: IDENT COLON typeRef COMMA?;
enumDecl: PUB? ENUM IDENT LBRACE enumVariantDecl* RBRACE;
enumVariantDecl: IDENT (LPAREN typeList? RPAREN)? COMMA?;
parameterList: parameter (COMMA parameter)* COMMA?;
parameter: IDENT COLON typeRef;
typeList: typeRef (COMMA typeRef)* COMMA?;
typeRef
    : primitiveType
    | qualifiedName
    | (OPTION | LIST | SENDER | RECEIVER | TASK) LBRACKET typeRef RBRACKET
    | (RESULT | MAP | LISTENER | CONNECTION) LBRACKET typeRef COMMA typeRef RBRACKET
    | LPAREN typeRef COMMA typeRef (COMMA typeRef)* COMMA? RPAREN
    ;
primitiveType: UNIT | BOOL | INT | FLOAT | STRING;

block: LBRACE statement* expression? RBRACE;
statement
    : (LET | VAR) bindingTarget (COLON typeRef)? ASSIGN expression SEMI # bindingStatement
    | IDENT ASSIGN expression SEMI                                      # assignmentStatement
    | expression SEMI                                                   # expressionStatement
    | whileExpression                                                   # whileStatement
    | forExpression                                                     # forStatement
    | loopExpression                                                    # loopStatement
    ;
bindingTarget: IDENT | LPAREN IDENT COMMA IDENT (COMMA IDENT)* COMMA? RPAREN;

expression: controlExpression | range;
controlExpression: RETURN expression? | BREAK | CONTINUE;
range: logicalOr (DOTDOT logicalOr)?;
logicalOr: logicalAnd (OR logicalAnd)*;
logicalAnd: equality (AND equality)*;
equality: comparison ((EQ | NE) comparison)*;
comparison: additive ((LT | LE | GT | GE) additive)*;
additive: multiplicative ((PLUS | MINUS) multiplicative)*;
multiplicative: unary ((STAR | SLASH | PERCENT) unary)*;
unary: (BANG | MINUS) unary | SPAWN postfix | update;
update: postfix (WITH recordBody)?;
postfix: primary postfixSuffix*;
postfixSuffix
    : typeArguments? LPAREN argumentList? RPAREN
    | DOT IDENT
    | DOT INT_LITERAL
    | QUESTION
    ;
argumentList: expression (COMMA expression)* COMMA?;
primary
    : integerLiteral
    | FLOAT_LITERAL
    | TRUE
    | FALSE
    | STRING_LITERAL
    | recordExpression
    | genericConstructor
    | CHANNEL
    | INT DOT IDENT
    | qualifiedName
    | LPAREN RPAREN
    | LPAREN expression RPAREN
    | tupleExpression
    | listExpression
    | ifExpression
    | matchExpression
    | whileExpression
    | forExpression
    | loopExpression
    | block
    ;
tupleExpression: LPAREN expression COMMA expression (COMMA expression)* COMMA? RPAREN;
listExpression: LBRACKET argumentList? RBRACKET;
recordExpression: qualifiedName recordBody;
recordBody: LBRACE recordFieldValue* RBRACE;
recordFieldValue: IDENT COLON expression COMMA?;
genericConstructor: (LIST | MAP) LBRACKET typeList RBRACKET LPAREN RPAREN;
typeArguments: LBRACKET typeList RBRACKET;
ifExpression: IF expression block (ELSE (ifExpression | block))?;
matchExpression: MATCH expression LBRACE matchArm* RBRACE;
matchArm: pattern FAT_ARROW expression COMMA?;
pattern
    : TRUE
    | FALSE
    | integerLiteral
    | STRING_LITERAL
    | qualifiedName (LPAREN patternList? RPAREN)?
    ;
patternList: pattern (COMMA pattern)* COMMA?;
whileExpression: WHILE expression block;
forExpression: FOR IDENT IN expression block;
loopExpression: LOOP block;
qualifiedName: IDENT (DOT IDENT)*;
integerLiteral: INT_LITERAL;
