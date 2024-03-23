from parglare import Parser, Grammar
from parglare.actions import pass_nochange, pass_single, pass_inner

# sys.setrecursionlimit(2000)

# grammar = Grammar.from_file("fleaux_grammar.pg")
grammar = Grammar.from_string('''
program:
  statement_list?;

statement_list:
  statement+;

statement:
  (let_statement | expression_statement) ';';

let_statement:
  LET identifier '(' parameter_decl_list ')' ':' type LET_EXPR_SEPARATOR expression;

expression_statement:
  expression;

parameter_decl_list:
  parameter+[COMMA];

parameter:
  identifier ':' type;

tuple_type:
  type_list;

type_list:
  type+[COMMA];

type:
  NUMBER |
  STRING | 
  BOOL | 
  NULL | 
  ANY | 
  identifier | 
  qualified_id | 
  '(' type_list ')';

expression:
  term |
  term PIPELINE_OPERATOR expression {left, 10} |
  term PLUS_OPERATOR expression {left, 5} |
  term MINUS_OPERATOR expression {left, 5} |
  term DIVIDE_OPERATOR expression {left, 6} |
  term MULTIPLY_OPERATOR expression {left, 6} |
  term MODULUS_OPERATOR expression {left, 6} |
  term EXPONENT_OPERATOR expression {right, 7} |
  term EQ_OPERATOR expression {left, 4} |
  term NE_OPERATOR expression {left, 4} |
  term LT_OPERATOR expression {left, 4} |
  term GT_OPERATOR expression {left, 4} |
  term GE_OPERATOR expression {left, 4} |
  term LE_OPERATOR expression {left, 4} |
  term OR_OPERATOR expression {left, 1} |
  term AND_OPERATOR expression {left, 2} |
  term COMMA expression {left, 1} |
  NOT_OPERATOR expression {right, 3} |
  PLUS_OPERATOR expression {right, 1} |
  MINUS_OPERATOR expression {right, 1};

term:
  constant | 
  qualified_id | 
  identifier | 
  parenthesized_expression;

parenthesized_expression: '(' expression ')';

qualified_id:
  identifier PERIOD identifier;

identifier:
  ID;

constant:
  NUMERICAL_CONSTANT | STRING_LITERAL | TRUE | FALSE | NULL;

LAYOUT: LayoutItem | LAYOUT LayoutItem | EMPTY;
LayoutItem: WS | Comment;

terminals
NUMERICAL_CONSTANT:
  /-?\d+(\.\d+)?(e|E[-+]?\d+)?/;

STRING_LITERAL: /"((\\")|[^"])*"/;
ID: /[a-zA-Z_][a-zA-Z_0-9]*/;

PIPELINE_OPERATOR: "->";
LE_OPERATOR: "<=";
GE_OPERATOR: ">=";
EQ_OPERATOR: "==";
NE_OPERATOR: "!=";
LT_OPERATOR: "<";
GT_OPERATOR: ">";
AND_OPERATOR: "&&";
OR_OPERATOR: "||";

PLUS_OPERATOR: "+";
MINUS_OPERATOR: "-";
DIVIDE_OPERATOR: "/";
MULTIPLY_OPERATOR: "*";
MODULUS_OPERATOR: "%";
EXPONENT_OPERATOR: "^";

NOT_OPERATOR: "!";

LET_EXPR_SEPARATOR: "::";

LET: "let";
NUMBER: "Number";
STRING: "String";
BOOL: "Bool";
NULL: "Null";
TRUE: "True";
FALSE: "False";
ANY: "Any";
COMMA: ",";
PERIOD: ".";

WS: /\s+/;
Comment: /\/\/.*/;
''')

actions = {
    "parenthesized_expression": [pass_inner]
}

parser = Parser(grammar, build_tree=True, actions=actions)

result = parser.parse('''
// let Multiply2(x : Number) : Number :: (x) -> Std.Multiply -> Std.Println; 
// let AddPrint(x : Number, y: Number) : Number :: (x, y) -> Std.Add -> Std.Println;
// 
// y = 4x^7 - x^5
// let Polynomial(x : Number) : Number :: ((((4, x) -> Std.Multiply, 7) -> Std.Pow), ((x, 5) -> Std.Pow)) -> Std.Subtract;
// let Polynomial2(x : Number) : Number :: ((4*x)^7) - (x^5);
// (1, 2) -> AddPrint;
// ("Hello, World!") -> Println;

2 -> ((((4, x) -> Std.Multiply, 7) -> Std.Pow), ((x, 5) -> Std.Pow)) -> Std.Subtract;
''')

print (type(result))
for stmt in result:
    print(stmt)

# parglare.visitor(result, )

# for statement in result:
#     print(statement)
