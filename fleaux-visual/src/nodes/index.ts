import { LiteralNode } from './LiteralNode';
import { LetNode } from './LetNode';
import { ClosureNode } from './ClosureNode';
import { ImportNode } from './ImportNode';
import { TupleNode } from './TupleNode';
import { StdValueNode } from './StdValueNode';
import { StdFuncNode } from './StdFuncNode';
import { UserFuncNode } from './UserFuncNode';

export const nodeTypes = {
  literalNode: LiteralNode,
  letNode: LetNode,
  closureNode: ClosureNode,
  importNode: ImportNode,
  tupleNode: TupleNode,
  stdValueNode: StdValueNode,
  stdFuncNode: StdFuncNode,
  userFuncNode: UserFuncNode,
} as const;
