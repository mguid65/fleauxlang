import functools
import sys
import structlog
import inspect
import logging
# import struct
import typing
from pathlib import Path

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


class Expression:
    def __init__(self):
        pass

    def __str__(self):
        return f''


class ExpressionBuilder:
    def __init__(self):
        pass


class OptionallyQualifiedIdentifier:
    def __init__(self, name: str = '', qualifier: str = "__UQ_GLOBAL__"):
        self.name: str = name
        self.qualifier: str = qualifier

    def set_name(self, name: str):
        self.name: str = name

    def set_qualifier(self, qualifier: str):
        self.qualifier: str = qualifier

    def __str__(self):
        return f'q{self.qualifier}_n{self.name}'


materialized_func_template = '''
def __fleaux_materialized_func_{origin_name}_{unique_func_id}({params}) -> {rtype}:
    return {expr}
'''
materialized_func_name_template = '__fleaux_materialized_func_{origin_name}_{unique_func_id}'

fleaux_inner_func_counter: int = 0


def gen_fleaux_inner_func(ident: OptionallyQualifiedIdentifier, params: ParameterList, rtype: Type, expr: Expression):
    global fleaux_inner_func_counter

    next_id = fleaux_inner_func_counter
    fleaux_inner_func_counter += 1

    return (materialized_func_name_template.format(origin_name=str(ident), unique_func_id=next_id),
            materialized_func_template.format(origin_name=str(ident), unique_func_id=next_id, params=str(params),
                                              rtype=str(rtype), expr=str(expr)))


class LetStatement:
    def __init__(self, ident: OptionallyQualifiedIdentifier, params: ParameterList, rtype: Type, expr: Expression):
        self.ident = ident
        self.params = params
        self.rtype = rtype
        self.expr = expr

    def set_ident(self, ident: OptionallyQualifiedIdentifier):
        self.ident = ident

    def set_params(self, params: ParameterList):
        self.params = params

    def set_rtype(self, rtype: Type):
        self.rtype = rtype

    def set_expr(self, expr: Expression):
        self.expr = expr

    def __str__(self):
        materialized_func_name, materialized_func = gen_fleaux_inner_func(self.ident, self.params, self.rtype,
                                                                          self.expr)
        return '{}\n\n{} = {}'.format(materialized_func, self.ident, materialized_func)


class ImportStatement:
    def __init__(self, gen_module_name: str):
        self.gen_module_name: str = gen_module_name

    def __str__(self):
        return f'import {self.gen_module_name}'


class ExpressionStatement:
    def __init__(self, expr: Expression):
        self.expr = expr

    def __str__(self):
        return str(self.expr)


class Statement:
    def __init__(self, stmt: ImportStatement | LetStatement | ExpressionStatement):
        self.stmt: ImportStatement | LetStatement | ExpressionStatement = stmt

    def __str__(self):
        return str(self.stmt)


class StatementList:
    def __init__(self):
        self.statements: list[Statement] = []

    def add_statement(self, stmt: Statement):
        self.statements.append(stmt)

    def __str__(self):
        return '\n'.join([str(s) for s in self.statements])


class FleauxTranspiler:
    def __init__(self, modules: dict[str, ImportStatement] = None):
        self.translation_unit: StatementList = StatementList()
        self.logger = structlog.get_logger()
        self.fleaux_mm = metamodel_from_file('fleaux_grammar.tx', auto_init_attributes=False)
        self.fleaux_mm.register_obj_processors({
            # 'Statement': self.handle_stmt,
            'ImportStatement': self.handle_import_stmt,
            'LetStatement': self.handle_let_stmt,
            'ExpressionStatement': self.handle_expr_stmt,
            'Expression': self.handle_expr,
            'ParameterDeclList': self.handle_param_list,
        })
        if modules is None:
            modules = {'Std': ImportStatement('fleaux_std_lib')}
        self.modules_seen: dict[str, ImportStatement] = modules

    def process(self, filename):
        logger.info()
        # self.variables = {}
        self.fleaux_mm.model_from_file(filename)

        filename_without_ext = Path(filename).with_suffix('')

        return f'fleaux_generated_module_{filename_without_ext}.py'

    def handle_import_stmt(self, import_stmt):
        logger.info(import_stmt)

        if import_stmt.module_name not in self.modules_seen:
            module_transpiler = FleauxTranspiler(modules=self.modules_seen)
            gen_py_mod_name = module_transpiler.process(f'{import_stmt.module_name}.fleaux')
            # maybe dangerous, probably change to something like
            mod_name = Path(gen_py_mod_name).with_suffix('')
            self.modules_seen[import_stmt.module_name] = ImportStatement(str(mod_name))
        self.translation_unit.add_statement(Statement(self.modules_seen[import_stmt.module_name]))

    def handle_let_stmt(self, let_stmt):
        logger.info(let_stmt)
        # print(let_stmt.definition)

    def handle_expr_stmt(self, expr_stmt):
        logger.info(expr_stmt)

    def handle_expr(self, expr):
        logger.info()

    def handle_param_list(self, param_list):
        logger.info()

    def handle_let_expr_def(self, expr_definition):
        logger.info()

    def handle_let_var_def(self, var_definition):
        logger.info()


transpiler = FleauxTranspiler()
transpiler.process('test.fleaux')

print(str(transpiler.translation_unit))
