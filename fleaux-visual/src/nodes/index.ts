import { LiteralNode } from './LiteralNode';
import { LetNode } from './LetNode';
import { ClosureNode } from './ClosureNode';
import { ImportNode } from './ImportNode';
import { TypeNode } from './TypeNode';
import { AliasNode } from './AliasNode';
import { TupleNode } from './TupleNode';
import { StdValueNode } from './StdValueNode';
import { StdFuncNode } from './StdFuncNode';
import { UserFuncNode } from './UserFuncNode';
import RawSourceNode from './RawSourceNode';
import { WildcardNode } from './WildcardNode';
import { SpreadNode } from './SpreadNode';
import { CallValueNode } from './CallValueNode';

export const nodeTypes = {
  literalNode: LiteralNode,
  letNode: LetNode,
  closureNode: ClosureNode,
  importNode: ImportNode,
  typeNode: TypeNode,
  aliasNode: AliasNode,
  rawSourceNode: RawSourceNode,
  tupleNode: TupleNode,
  spreadNode: SpreadNode,
  stdValueNode: StdValueNode,
  stdFuncNode: StdFuncNode,
  userFuncNode: UserFuncNode,
  callValueNode: CallValueNode,
  wildcardNode: WildcardNode,
} as const;
