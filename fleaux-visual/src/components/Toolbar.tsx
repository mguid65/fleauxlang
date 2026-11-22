import { useCallback, useRef, useState, type CSSProperties } from 'react';
import { useFlowStore } from '../store/flowStore';
import type { FleauxNodeData } from '../lib/types';
import { useStoreApi, type Node } from '@xyflow/react';
import { extractUserAliases, extractUserFunctions, extractUserTypes } from '../lib/userFunctions';
import {
  STD_BY_NAMESPACE,
  STD_FUNCTIONS,
  STD_VALUES,
  STD_VALUES_BY_NAMESPACE,
  type StdFunctionEntry,
  type StdValueEntry,
} from '../lib/stdCatalogue';
import {
  formatAritySummary,
  formatCompactFunctionLabel,
  formatFunctionDisplayName,
  formatFunctionDisplaySignature,
} from '../lib/functionSignatures';
import { STD_NAMESPACES_IN_ORDER, stdEntryStyle, stdTextStyle } from '../lib/stdTheme';
import { OPERATOR_CATEGORIES, OPERATORS, type OperatorEntry } from '../lib/operators';

function operatorFunction(entry: OperatorEntry): StdFunctionEntry | undefined {
  return STD_FUNCTIONS.find((candidate) => candidate.qualifiedName === entry.qualifiedName);
}

function OperatorButton({ entry, onAdd, textStyle }: { entry: OperatorEntry; onAdd: (entry: OperatorEntry) => void; textStyle: CSSProperties }) {
  const fn = operatorFunction(entry);
  return (
    <button
      style={{ ...textStyle, padding: '2px 2px 2px 3px' }}
      title={fn ? `${entry.token}: ${fn.displaySignature}` : entry.token}
      onClick={() => onAdd(entry)}
      className="text-left text-[11px] font-mono hover:bg-white/10 rounded transition-colors cursor-pointer"
    >
      <div>
        <span className="inline-block w-6 font-bold">{entry.token}</span>
        {fn ? formatCompactFunctionLabel(fn.name, fn) : entry.qualifiedName}
      </div>
      <div className="text-[9px] opacity-60">{entry.qualifiedName}</div>
    </button>
  );
}

function formatStdFunctionMeta(fn: StdFunctionEntry): string {
  const tags: string[] = [];

  if (fn.overloadCount > 1) {
    tags.push(`overload ${fn.overloadIndex}/${fn.overloadCount}`);
  }

  if (fn.hasVariadicTail) {
    tags.push(`variadic ${formatAritySummary(fn.params)}`);
  }

  if (fn.isTerminal) {
    tags.push('terminal');
  }

  return tags.join(' | ');
}

function uid() {
  return `node-${crypto.randomUUID()}`;
}

// Where the next node from the toolbar goes: the middle of the visible canvas, each one a step down and right of the
// last so that a few in a row do not cover each other.
function useNewNodePosition() {
  const store = useStoreApi();
  const step = useRef(0);
  return useCallback(() => {
    const { width, height, transform } = store.getState();
    const [panX, panY, zoom] = transform;
    const offset = (step.current++ % 8) * 24;
    return { x: (width / 2 - panX) / zoom - 80 + offset, y: (height / 2 - panY) / zoom - 60 + offset };
  }, [store]);
}

export function Toolbar() {
  const addNode = useFlowStore((s) => s.addNode);
  const generateSource = useFlowStore((s) => s.generateSource);
  const runGraphWithWasm = useFlowStore((s) => s.runGraphWithWasm);
  const wasmStatus = useFlowStore((s) => s.wasmStatus);
  const [openNs, setOpenNs] = useState<string | null>(null);
  const [query, setQuery] = useState('');
  const newNodePosition = useNewNodePosition();

  const toggle = (ns: string) => setOpenNs((prev) => (prev === ns ? null : ns));

  const addStdFunc = useCallback(
    (fn: StdFunctionEntry) => {
      addNode({
        id: uid(),
        type: 'stdFuncNode',
        position: newNodePosition(),
        data: {
          kind: 'stdFunc' as const,
          qualifiedName: fn.qualifiedName,
          namespace: fn.namespace,
          typeParams: fn.typeParams,
          params: fn.params,
          returnType: fn.returnType,
          signatureKey: fn.signatureKey,
          displayName: fn.displayName,
          displaySignature: fn.displaySignature,
          hasVariadicTail: fn.hasVariadicTail,
          minimumArity: fn.minimumArity,
          overloadIndex: fn.overloadIndex,
          overloadCount: fn.overloadCount,
          isTerminal: fn.isTerminal,
          label: fn.displayName,
        },
      } as Node<FleauxNodeData>);
    },
    [addNode, newNodePosition],
  );

  // An operator node is a call of its Std builtin that serializes as the operator.
  const addOperator = useCallback(
    (entry: OperatorEntry) => {
      const fn = STD_FUNCTIONS.find((candidate) => candidate.qualifiedName === entry.qualifiedName);
      if (!fn) {
        return;
      }
      addNode({
        id: uid(),
        type: 'stdFuncNode',
        position: newNodePosition(),
        data: {
          kind: 'stdFunc' as const,
          qualifiedName: fn.qualifiedName,
          namespace: fn.namespace,
          typeParams: fn.typeParams,
          params: fn.params,
          returnType: fn.returnType,
          signatureKey: fn.signatureKey,
          displayName: entry.token,
          displaySignature: fn.displaySignature,
          hasVariadicTail: fn.hasVariadicTail,
          minimumArity: fn.minimumArity,
          overloadIndex: fn.overloadIndex,
          overloadCount: fn.overloadCount,
          isTerminal: fn.isTerminal,
          operator: entry.token,
          label: entry.token,
        },
      } as Node<FleauxNodeData>);
    },
    [addNode, newNodePosition],
  );

  const addStdValue = useCallback(
    (value: StdValueEntry) => {
      addNode({
        id: uid(),
        type: 'stdValueNode',
        position: newNodePosition(),
        data: {
          kind: 'stdValue' as const,
          qualifiedName: value.qualifiedName,
          namespace: value.namespace,
          valueType: value.valueType,
          label: value.qualifiedName,
        },
      } as Node<FleauxNodeData>);
    },
    [addNode, newNodePosition],
  );

  const addUserFunc = useCallback(
    (
      funcName: string,
      nodeId: string,
      typeParams: string[] | undefined,
      params: { name: string; type: string }[],
      returnType: string,
      isValueBinding: boolean,
    ) => {
      addNode({
        id: uid(),
        type: 'userFuncNode',
        position: newNodePosition(),
        data: {
          kind: 'userFunc' as const,
          functionName: funcName,
          functionNodeId: nodeId,
          typeParams,
          params: params.map((param) => ({ ...param })),
          returnType,
          // A value let is used by its name, never called.
          isReference: isValueBinding,
          label: formatFunctionDisplayName(funcName, typeParams),
        },
      } as Node<FleauxNodeData>);
    },
    [addNode, newNodePosition],
  );

  const addPrimitive = useCallback(
    (partial: Omit<Node<FleauxNodeData>, 'id' | 'position'>) => {
      addNode({
        id: uid(),
        position: newNodePosition(),
        ...partial,
      } as Node<FleauxNodeData>);
    },
    [addNode, newNodePosition],
  );

  const lq = query.toLowerCase();
  const hasQuery = lq.length > 0;
  const nodes = useFlowStore((s) => s.nodes);
  const userFunctions = extractUserFunctions(nodes);
  const userTypes = extractUserTypes(nodes);
  const userAliases = extractUserAliases(nodes);
  const isRunningWasm = wasmStatus === 'running';

  return (
    <aside className="absolute top-4 left-4 z-10 flex flex-col gap-2 bg-raised border border-line rounded-xl shadow-xl w-52 max-h-[92vh] overflow-auto"
        style={{
            padding: '4px 8px 4px 4px'
        }}
    >
      <div className="text-xs font-bold text-neutral-400 uppercase tracking-widest mb-1">Nodes</div>

      {/*  Primitive nodes  */}
      <div className="text-[10px] text-neutral-500 uppercase tracking-wider">Primitives</div>
      <button onClick={() => addPrimitive({ type: 'importNode', data: { kind: 'import', moduleName: 'Std', label: 'import Std' } })}
        className="text-xs font-mono border border-teal-600 text-teal-300 hover:bg-teal-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Import</button>
      <button onClick={() => addPrimitive({ type: 'typeNode', data: { kind: 'typeDecl', name: 'MyType', targetType: 'Any', separator: '=', label: 'type MyType = Any' } })}
        className="text-xs font-mono border border-purple-600 text-purple-300 hover:bg-purple-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Type</button>
      <button onClick={() => addPrimitive({ type: 'aliasNode', data: { kind: 'aliasDecl', name: 'MyAlias', targetType: 'Any', label: 'alias MyAlias = Any' } })}
        className="text-xs font-mono border border-lime-600 text-lime-300 hover:bg-lime-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Alias</button>
      <button onClick={() => addPrimitive({ type: 'letNode', data: { kind: 'let', name: 'MyFunc', params: [], returnType: 'Any', label: 'let MyFunc' } })}
        className="text-xs font-mono border border-fuchsia-600 text-fuchsia-300 hover:bg-fuchsia-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Let</button>
      <button onClick={() => addPrimitive({ type: 'closureNode', data: { kind: 'closure', params: [{ name: 'x', type: 'Any' }], returnType: 'Any', label: '(x: Any): Any' } })}
        className="text-xs font-mono border border-violet-600 text-violet-300 hover:bg-violet-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Closure</button>
      <button onClick={() => addPrimitive({ type: 'tupleNode', data: { kind: 'tuple', arity: 2, label: '( _, _ )' } })}
        className="text-xs font-mono border border-orange-600 text-orange-300 hover:bg-orange-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Tuple</button>
      <button onClick={() => addPrimitive({ type: 'spreadNode', data: { kind: 'spread', label: 'x...' } })}
        className="text-xs font-mono border border-rose-600 text-rose-300 hover:bg-rose-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Spread</button>
      <button onClick={() => addPrimitive({ type: 'callValueNode', data: { kind: 'callValue', slots: 1, label: 'call value' } })}
        className="text-xs font-mono border border-teal-600 text-teal-300 hover:bg-teal-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Call value</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'String', value: '', label: '""' } })}
        className="text-xs font-mono border border-emerald-600 text-emerald-300 hover:bg-emerald-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ String</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Bool', value: 'True', label: 'True' } })}
        className="text-xs font-mono border border-amber-600 text-amber-300 hover:bg-amber-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Bool</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Null', value: 'null', label: 'null' } })}
        className="text-xs font-mono border border-pink-600 text-pink-300 hover:bg-pink-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Null</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Int64', value: '0', label: '0' } })}
        className="text-xs font-mono border border-green-600 text-green-300 hover:bg-green-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Int64</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'UInt64', value: '0', label: '0' } })}
        className="text-xs font-mono border border-yellow-600 text-yellow-300 hover:bg-yellow-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ UInt64</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Float64', value: '0.0', label: '0.0' } })}
        className="text-xs font-mono border border-orange-600 text-orange-300 hover:bg-orange-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Float64</button>

      {/*  Std functions  */}
      <div className="border-t border-line my-1" />

      {userFunctions.length > 0 && (
        <>
          <div className="text-[10px] text-neutral-500 uppercase tracking-wider mt-2">User Functions</div>
          {userFunctions.map((uf) => (
            <button
                style={{paddingLeft: '3px'}}
              key={uf.nodeId}
              onClick={() => addUserFunc(uf.name, uf.nodeId, uf.typeParams, uf.params, uf.returnType, uf.isValueBinding)}
              className="text-xs font-mono border border-purple-600 text-purple-300 hover:bg-purple-900 rounded transition-colors cursor-pointer text-left"
              title={uf.isValueBinding
                ? `${uf.name}: ${uf.returnType}`
                : formatFunctionDisplaySignature(uf.name, {
                  params: uf.params,
                  returnType: uf.returnType,
                  typeParams: uf.typeParams,
                })}
            >
              {/* A value let is a value, `P: Type`, not a call. */}
              + {uf.isValueBinding
                ? `${uf.name}: ${uf.returnType}`
                : formatCompactFunctionLabel(uf.name, {
                  params: uf.params,
                  returnType: uf.returnType,
                  typeParams: uf.typeParams,
                })}
            </button>
          ))}
          <div className="border-t border-line my-1" />
        </>
      )}

      {userTypes.length > 0 && (
        <>
          <div className="text-[10px] text-neutral-500 uppercase tracking-wider mt-2">User Types</div>
          {userTypes.map((entry) => (
            <div
              style={{paddingLeft: '3px'}}
              key={entry.nodeId}
              className="text-[11px] font-mono border border-purple-800 text-purple-200 bg-purple-950/30 rounded px-2 py-1"
              title={'type ' + entry.name + ' ' + entry.separator + ' ' + entry.targetType}
            >
              <div className="font-bold">{entry.name}</div>
              <div className="text-[10px] opacity-70">{entry.separator + ' ' + entry.targetType}</div>
            </div>
          ))}
          <div className="border-t border-line my-1" />
        </>
      )}

      {userAliases.length > 0 && (
        <>
          <div className="text-[10px] text-neutral-500 uppercase tracking-wider mt-2">User Aliases</div>
          {userAliases.map((entry) => (
            <div style={{paddingLeft: '3px'}}
              key={entry.nodeId}
              className="text-[11px] font-mono border border-lime-800 text-lime-200 bg-lime-950/30 rounded"
              title={'alias ' + entry.name + ' = ' + entry.targetType}
            >
              <div className="font-bold">{entry.name}</div>
              <div className="text-[10px] opacity-70">{'= ' + entry.targetType}</div>
            </div>
          ))}
          <div className="border-t border-line my-1" />
        </>
      )}

      {/*  Operators: their own section, one dropdown per category, opened like the Std namespaces.  */}
      <div className="text-[10px] text-neutral-500 uppercase tracking-wider">Operators</div>
      {OPERATOR_CATEGORIES.map((category) => {
        const key = `Operators: ${category}`;
        const entries = OPERATORS.filter((entry) => entry.category === category);
        const isOpen = openNs === key;
        return (
          <div key={key}>
            <button
              style={{ ...stdEntryStyle(category), paddingLeft: '3px' }}
              onClick={() => toggle(key)}
              className="w-full text-left text-[11px] font-mono border hover:bg-white/5 rounded px-2 py-1.5 transition-colors cursor-pointer flex justify-between items-center"
            >
              <span>{category}</span>
              <span className="opacity-50 text-[9px]">{isOpen ? '▲' : '▼'} {entries.length}</span>
            </button>
            {isOpen && (
              <div className="flex flex-col gap-0.5 mt-0.5 border-l border-line">
                {entries.map((entry) => (
                  <OperatorButton key={`${entry.token}/${entry.arity}`} entry={entry} onAdd={addOperator} textStyle={stdTextStyle(category)} />
                ))}
              </div>
            )}
          </div>
        );
      })}
      <div className="border-t border-line my-1" />

      <div className="text-[10px] text-neutral-500 uppercase tracking-wider">Standard Library</div>

      <input
        style={{paddingLeft: '3px'}}
        type="text"
        placeholder="Filter functions…"
        value={query}
        onChange={(e) => setQuery(e.target.value)}
        className="text-xs font-mono bg-panel/50 border border-line rounded px-2 py-1 text-neutral-200 placeholder-neutral-600 outline-none focus:border-neutral-500"
      />

      {hasQuery ? (
        <div className="flex flex-col gap-1">
          {STD_VALUES.filter((value) =>
            value.qualifiedName.toLowerCase().includes(lq) || value.name.toLowerCase().includes(lq),
          ).map((value) => {
            return (
              <button
                style={{ ...stdEntryStyle(value.namespace), paddingLeft: '3px' }}
                key={value.qualifiedName}
                title={`${value.qualifiedName}: ${value.valueType}`}
                onClick={() => addStdValue(value)}
                className="text-left text-[11px] font-mono border hover:bg-white/10 rounded transition-colors cursor-pointer"
              >
                {value.qualifiedName}
              </button>
            );
          })}
          {STD_FUNCTIONS.filter((fn) =>
            fn.qualifiedName.toLowerCase().includes(lq)
              || fn.displaySignature.toLowerCase().includes(lq)
              || fn.name.toLowerCase().includes(lq),
          ).map((fn) => {
            const meta = formatStdFunctionMeta(fn);
            return (
              <button
                style={{ ...stdEntryStyle(fn.namespace), paddingLeft: '3px' }}
                key={fn.signatureKey}
                title={fn.displaySignature}
                onClick={() => addStdFunc(fn)}
                className="text-left text-[11px] font-mono border hover:bg-white/10 rounded px-2 py-1 transition-colors cursor-pointer"
              >
                <div>{fn.displaySignature}</div>
                {meta.length > 0 && <div className="text-[9px] opacity-60">{meta}</div>}
              </button>
            );
          })}
        </div>
      ) : (
        STD_NAMESPACES_IN_ORDER.map((ns) => {
          const values = STD_VALUES_BY_NAMESPACE.get(ns) ?? [];
          const fns = STD_BY_NAMESPACE.get(ns) ?? [];
          const isOpen = openNs === ns;
          return (
            <div key={ns}>
              <button
                style={{ ...stdEntryStyle(ns), paddingLeft: '3px' }}
                onClick={() => toggle(ns)}
                className="w-full text-left text-[11px] font-mono border hover:bg-white/5 rounded px-2 py-1.5 transition-colors cursor-pointer flex justify-between items-center"
              >
                <span>{ns}</span>
                <span className="opacity-50 text-[9px]">{isOpen ? '▲' : '▼'} {values.length + fns.length}</span>
              </button>
              {isOpen && (
                <div className="flex flex-col gap-0.5 mt-0.5 border-l border-line">
                  {values.map((value) => (
                    <button
                      style={{ ...stdTextStyle(ns), padding: '2px 2px 2px 3px', verticalAlign: 'center' }}
                      key={value.qualifiedName}
                      title={`${value.qualifiedName}: ${value.valueType}`}
                      onClick={() => addStdValue(value)}
                      className="text-left text-[11px] font-mono hover:bg-white/10 rounded transition-colors cursor-pointer"
                    >
                      {value.name}
                    </button>
                  ))}
                  {fns.map((fn) => (
                    <button
                      style={{ ...stdTextStyle(ns), padding: '2px 2px 2px 3px', verticalAlign: 'center' }}
                      key={fn.signatureKey}
                      title={fn.displaySignature}
                      onClick={() => addStdFunc(fn)}
                      className="text-left text-[11px] font-mono hover:bg-white/10 rounded transition-colors cursor-pointer"
                    >
                      <div>{formatCompactFunctionLabel(fn.name, fn)}</div>
                      {formatStdFunctionMeta(fn).length > 0 && (
                        <div className="text-[9px] opacity-60">{formatStdFunctionMeta(fn)}</div>
                      )}
                    </button>
                  ))}
                </div>
              )}
            </div>
          );
        })
      )}

      <div className="border-t border-line my-1" />
      <div className="text-[10px] text-neutral-500 uppercase tracking-wider">Graph</div>
      <button
        onClick={generateSource}
        disabled={isRunningWasm}
        title="Write the graph's source into the editor"
        className="text-xs font-mono border border-emerald-700 text-emerald-300 hover:bg-emerald-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
      >
        Generate Source
      </button>
      <button
        onClick={() => void runGraphWithWasm()}
        disabled={isRunningWasm}
        title="Generate the graph's source and run it with Fleaux WASM"
        className="text-xs font-mono border border-amber-700 text-amber-300 hover:bg-amber-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
      >
        {isRunningWasm ? 'Running…' : 'Run Graph'}
      </button>

    </aside>
  );
}
