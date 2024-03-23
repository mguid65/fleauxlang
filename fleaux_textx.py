import functools
import sys
import structlog
import inspect
import logging
# import struct
import typing

from textx.metamodel import metamodel_from_file

structlog.configure(
    processors=[
        # If log level is too low, abort pipeline and throw away log entry.
        structlog.stdlib.filter_by_level,
        # Add the name of the logger to event dict.
        # structlog.stdlib.add_logger_name,
        # Add log level to event dict.
        structlog.stdlib.add_log_level,
        # Perform %-style formatting.
        structlog.stdlib.PositionalArgumentsFormatter(),
        # Add a timestamp in ISO 8601 format.
        structlog.processors.TimeStamper(fmt="iso"),
        # If the "stack_info" key in the event dict is true, remove it and
        # render the current stack trace in the "stack" key.
        structlog.processors.StackInfoRenderer(),
        # If the "exc_info" key in the event dict is either true or a
        # sys.exc_info() tuple, remove "exc_info" and render the exception
        # with traceback into the "exception" key.
        structlog.processors.format_exc_info,
        # If some value is in bytes, decode it to a Unicode str.
        structlog.processors.UnicodeDecoder(),
        # Add callsite parameters.
        structlog.processors.CallsiteParameterAdder(
            {
                structlog.processors.CallsiteParameter.FILENAME,
                structlog.processors.CallsiteParameter.FUNC_NAME,
                structlog.processors.CallsiteParameter.LINENO,
            }
        ),
        # Render the final event dict as JSON.
        structlog.processors.JSONRenderer()
    ],
    # `wrapper_class` is the bound logger that you get back from
    # get_logger(). This one imitates the API of `logging.Logger`.
    wrapper_class=structlog.stdlib.BoundLogger,
    # `logger_factory` is used to create wrapped loggers that are used for
    # OUTPUT. This one returns a `logging.Logger`. The final value (a JSON
    # string) from the final processor (`JSONRenderer`) will be passed to
    # the method of the same name as that you've called on the bound logger.
    logger_factory=structlog.stdlib.LoggerFactory(),
    # Effectively freeze configuration after creating the first bound
    # logger.
    cache_logger_on_first_use=True,
)

logging.basicConfig(
    format="%(message)s",
    stream=sys.stdout,
    level=logging.DEBUG,
)

logger = structlog.get_logger()


class Type:
    def __init__(self, name: str = ''):
        self.name: str = name

    def __str__(self):
        return self.name


class Parameter:
    def __init__(self, name: str = '', ptype: str = ''):
        self.name: str = name
        self.type: str = ptype

    def __str__(self):
        return f'{self.name} : {self.type}'

    def set_name(self, name: str):
        self.name = name

    def set_type(self, ptype: str):
        self.type = ptype

    def set(self, name: str, ptype: str):
        self.name = name
        self.type = ptype


class ParameterList:
    def __init__(self, parameters=None):
        if parameters is None:
            parameters = []
        self.parameters: list[Parameter] = parameters

    def __str__(self):
        return ', '.join([str(p) for p in self.parameters])

    def add_parameter(self, param: Parameter):
        self.parameters.append(param)


# p_list = ParameterList([Parameter("x", "float"), Parameter("y", "float"), Parameter("z", "float")])
#
# print(p_list)


class Expression:
    def __init__(self):
        pass

    def __str__(self):
        return f''


class ExpressionBuilder:
    def __init__(self):
        pass


materialized_func_template = '''
def __fleaux_materialized_func_{origin_name}_{id}({params}) -> {rtype}:
    return {expr}
'''


class MaterializedFunction:
    counter: int = 0

    @staticmethod
    def next_id():
        val = MaterializedFunction.counter
        MaterializedFunction.counter += 1
        return val

    def __init__(self, ):
        self.origin_function_name: str = ''
        self.param_list: ParameterList = ParameterList()
        self.expr: Expression = Expression()
        self.rtype: Type = Type()

        self._id: int = MaterializedFunction.next_id()

    def __str__(self) -> str:
        return materialized_func_template.format(origin_name=self.origin_function_name, id=self._id,
                                                 params=str(self.param_list), rtype=str(self.rtype),
                                                 expr=str(self.expr))

    def set_origin_function_name(self, origin_function_name: str):
        self.origin_function_name = origin_function_name

    def set_parameter_list(self, params: ParameterList):
        self.param_list = params

    def set_rtype(self, rtype: Type):
        self.rtype = rtype

    def set_expression(self, expr: Expression):
        self.expr = expr


class LetFunctionDefinition:
    def __init__(self):
        pass

    def __str__(self):
        return ''


class LetVariableDefinition:
    def __init__(self):
        pass

    def __str__(self):
        return ''


class OptionallyQualifiedIdentifier:
    def __init__(self, name: str = '', qualifier: str = None):
        self.name: str = name
        self.qualifier: str = qualifier


class LetStatement:
    def __init__(self, ident: OptionallyQualifiedIdentifier = OptionallyQualifiedIdentifier(),
                 definition: LetVariableDefinition | LetFunctionDefinition = None):
        self.identifier: OptionallyQualifiedIdentifier = ident
        self.statement_rhs: LetVariableDefinition | LetFunctionDefinition = definition

    def __str__(self):
        if isinstance(self.statement_rhs, LetVariableDefinition):
            return '{} {}'
        if isinstance(self.statement_rhs, LetFunctionDefinition):
            return '{} {}'


class ImportStatement:
    def __init__(self, module_name: str):
        self.module_name: str = module_name

    def __str__(self):
        return 'import {}'.format(self.module_name)


class Statement:
    def __init__(self):
        pass


class StatementList:
    def __init__(self):
        self.statements: list[Statement] = []

    def __str__(self):
        return '\n'.join([str(s) for s in self.statements])


class MaterializedTemporaryBuilder:
    def __init__(self):
        pass


class LetFunctionExpressionBuilder:
    def __init__(self):
        pass


class LetVariableExpressionBuilder:
    def __init__(self):
        pass


class FleauxTranspiler:
    def __init__(self):
        self.logger = structlog.get_logger()
        self.fleaux_mm = metamodel_from_file('fleaux_grammar.tx', auto_init_attributes=False)
        self.fleaux_mm.register_obj_processors({
            'ImportStatement': self.handle_import_stmt,
            'LetStatement': self.handle_let_stmt,
            'LetExpressionDefinition': self.handle_let_expr_def,
            'LetVariableDefinition': self.handle_let_var_def,
            'ExpressionStatement': self.handle_expr_stmt,
            # 'Expression': self.handle_expr,
            # 'ParameterDeclList': self.handle_param_list,

        })

    def process(self, filename):
        logger.info()
        # self.variables = {}
        self.fleaux_mm.model_from_file(filename)

    def handle_import_stmt(self, import_stmt):
        logger.info()
        # print(import_stmt.module_name)

    def handle_let_stmt(self, let_stmt):
        logger.info()
        # print(let_stmt.definition)

    def handle_expr_stmt(self, expr_stmt):
        logger.info()

    def handle_expr(self, expr):
        logger.info()

    def handle_param_list(self, param_list):
        logger.info()

    def handle_let_expr_def(self, expr_definition):
        logger.info()

    def handle_let_var_def(self, var_definition):
        logger.info()


transpiler = FleauxTranspiler()
transpiler.process('Std.fleaux')