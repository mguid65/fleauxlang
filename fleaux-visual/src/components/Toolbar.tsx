import { useCallback, useRef, useState, type ChangeEvent } from 'react';
import { useFlowStore } from '../store/flowStore';
import type { FleauxNodeData } from '../lib/types';
import type { Node } from '@xyflow/react';
import { extractUserFunctions } from '../lib/userFunctions';
import {
  STD_BY_NAMESPACE,
  STD_FUNCTIONS,
  STD_NAMESPACES,
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
import { NS_BORDER, NS_COLORS, NS_TEXT } from '../lib/stdTheme';

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

let nodeCounter = 100;
function uid() {
  return `node-${++nodeCounter}`;
}

export function Toolbar() {
  const addNode = useFlowStore((s) => s.addNode);
  const clearGraph = useFlowStore((s) => s.clearGraph);
  const loadGraphFromSource = useFlowStore((s) => s.loadGraphFromSource);
  const runGraphWithWasm = useFlowStore((s) => s.runGraphWithWasm);
  const wasmStatus = useFlowStore((s) => s.wasmStatus);
  const [openNs, setOpenNs] = useState<string | null>(null);
  const [query, setQuery] = useState('');
  const fileInputRef = useRef<HTMLInputElement | null>(null);

  const toggle = (ns: string) => setOpenNs((prev) => (prev === ns ? null : ns));

  const addStdFunc = useCallback(
    (fn: StdFunctionEntry) => {
      addNode({
        id: uid(),
        type: 'stdFuncNode',
        position: { x: 220 + Math.random() * 300, y: 160 + Math.random() * 220 },
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
    [addNode],
  );

  const addStdValue = useCallback(
    (value: StdValueEntry) => {
      addNode({
        id: uid(),
        type: 'stdValueNode',
        position: { x: 220 + Math.random() * 300, y: 160 + Math.random() * 220 },
        data: {
          kind: 'stdValue' as const,
          qualifiedName: value.qualifiedName,
          namespace: value.namespace,
          valueType: value.valueType,
          label: value.qualifiedName,
        },
      } as Node<FleauxNodeData>);
    },
    [addNode],
  );

  const addUserFunc = useCallback(
    (
      funcName: string,
      nodeId: string,
      typeParams: string[] | undefined,
      params: { name: string; type: string }[],
      returnType: string,
    ) => {
      addNode({
        id: uid(),
        type: 'userFuncNode',
        position: { x: 220 + Math.random() * 300, y: 160 + Math.random() * 220 },
        data: {
          kind: 'userFunc' as const,
          functionName: funcName,
          functionNodeId: nodeId,
          typeParams,
          params,
          returnType,
          label: formatFunctionDisplayName(funcName, typeParams),
        },
      } as Node<FleauxNodeData>);
    },
    [addNode],
  );

  const addPrimitive = useCallback(
    (partial: Omit<Node<FleauxNodeData>, 'id' | 'position'>) => {
      addNode({
        id: uid(),
        position: { x: 220 + Math.random() * 300, y: 160 + Math.random() * 220 },
        ...partial,
      } as Node<FleauxNodeData>);
    },
    [addNode],
  );

  const lq = query.toLowerCase();
  const hasQuery = lq.length > 0;
  const nodes = useFlowStore((s) => s.nodes);
  const userFunctions = extractUserFunctions(nodes);
  const isRunningWasm = wasmStatus === 'running';
  const handleLoadFile = async (evt: ChangeEvent<HTMLInputElement>) => {
    const file = evt.target.files?.[0];
    if (!file) {
      return;
    }

    try {
      const source = await file.text();
      loadGraphFromSource(source);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      console.error(`Failed to load Fleaux file: ${message}`);
    } finally {
      evt.target.value = '';
    }
  };

  return (
    <aside className="absolute top-4 left-4 z-10 flex flex-col gap-2 bg-[#1a1d2e] border border-[#2d3148] rounded-xl p-3 shadow-xl w-52 max-h-[92vh] overflow-auto">
      <div className="text-xs font-bold text-slate-400 uppercase tracking-widest mb-1">Add Node</div>

      {/*  Primitive nodes  */}
      <div className="text-[10px] text-slate-500 uppercase tracking-wider">Primitives</div>
      <button onClick={() => addPrimitive({ type: 'importNode', data: { kind: 'import', moduleName: 'Std', label: 'import Std' } })}
        className="text-xs font-mono border border-teal-600 text-teal-300 hover:bg-teal-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Import</button>
      <button onClick={() => addPrimitive({ type: 'letNode', data: { kind: 'let', name: 'MyFunc', params: [], returnType: 'Any', label: 'let MyFunc' } })}
        className="text-xs font-mono border border-fuchsia-600 text-fuchsia-300 hover:bg-fuchsia-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Let</button>
      <button onClick={() => addPrimitive({ type: 'closureNode', data: { kind: 'closure', params: [{ name: 'x', type: 'Any' }], returnType: 'Any', label: '(x: Any): Any' } })}
        className="text-xs font-mono border border-violet-600 text-violet-300 hover:bg-violet-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Closure</button>
      <button onClick={() => addPrimitive({ type: 'tupleNode', data: { kind: 'tuple', arity: 2, label: '( _, _ )' } })}
        className="text-xs font-mono border border-orange-600 text-orange-300 hover:bg-orange-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Tuple</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'String', value: '', label: '""' } })}
        className="text-xs font-mono border border-emerald-600 text-emerald-300 hover:bg-emerald-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ String</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Bool', value: 'True', label: 'True' } })}
        className="text-xs font-mono border border-amber-600 text-amber-300 hover:bg-amber-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Bool</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Null', value: 'null', label: 'null' } })}
        className="text-xs font-mono border border-slate-600 text-slate-300 hover:bg-slate-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Null</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Int64', value: '0', label: '0' } })}
        className="text-xs font-mono border border-cyan-600 text-cyan-300 hover:bg-cyan-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Int64</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'UInt64', value: '0', label: '0' } })}
        className="text-xs font-mono border border-indigo-600 text-indigo-300 hover:bg-indigo-950 rounded px-3 py-1.5 transition-colors cursor-pointer">+ UInt64</button>
      <button onClick={() => addPrimitive({ type: 'literalNode', data: { kind: 'literal', valueType: 'Float64', value: '0.0', label: '0.0' } })}
        className="text-xs font-mono border border-sky-600 text-sky-300 hover:bg-sky-900 rounded px-3 py-1.5 transition-colors cursor-pointer">+ Float64</button>

      {/*  Std functions  */}
      <div className="border-t border-[#2d3148] my-1" />

      {userFunctions.length > 0 && (
        <>
          <div className="text-[10px] text-slate-500 uppercase tracking-wider mt-2">User Functions</div>
          {userFunctions.map((uf) => (
            <button
              key={uf.nodeId}
              onClick={() => addUserFunc(uf.name, uf.nodeId, uf.typeParams, uf.params, uf.returnType)}
              className="text-xs font-mono border border-purple-600 text-purple-300 hover:bg-purple-900 rounded px-3 py-1.5 transition-colors cursor-pointer text-left"
              title={formatFunctionDisplaySignature(uf.name, {
                params: uf.params,
                returnType: uf.returnType,
                typeParams: uf.typeParams,
              })}
            >
              + {formatCompactFunctionLabel(uf.name, {
                params: uf.params,
                returnType: uf.returnType,
                typeParams: uf.typeParams,
              })}
            </button>
          ))}
          <div className="border-t border-[#2d3148] my-1" />
        </>
      )}

      <div className="text-[10px] text-slate-500 uppercase tracking-wider">Standard Library</div>

      <input
        type="text"
        placeholder="Filter functions…"
        value={query}
        onChange={(e) => setQuery(e.target.value)}
        className="text-xs font-mono bg-[#12142080] border border-[#2d3148] rounded px-2 py-1 text-slate-200 placeholder-slate-600 outline-none focus:border-slate-500"
      />

      {hasQuery ? (
        <div className="flex flex-col gap-1">
          {STD_VALUES.filter((value) =>
            value.qualifiedName.toLowerCase().includes(lq) || value.name.toLowerCase().includes(lq),
          ).map((value) => {
            const nsColor = NS_COLORS[value.namespace] ?? '';
            const border = nsColor.split(' ')[0] ?? 'border-slate-700';
            return (
              <button
                key={value.qualifiedName}
                title={`${value.qualifiedName}: ${value.valueType}`}
                onClick={() => addStdValue(value)}
                className={`text-left text-[11px] font-mono border ${border} text-slate-200 hover:bg-white/10 rounded px-2 py-1 transition-colors cursor-pointer`}
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
            const nsColor = NS_COLORS[fn.namespace] ?? '';
            const border = nsColor.split(' ')[0] ?? 'border-slate-700';
            const meta = formatStdFunctionMeta(fn);
            return (
              <button
                key={fn.signatureKey}
                title={fn.displaySignature}
                onClick={() => addStdFunc(fn)}
                className={`text-left text-[11px] font-mono border ${border} text-slate-200 hover:bg-white/10 rounded px-2 py-1 transition-colors cursor-pointer`}
              >
                <div>{fn.displaySignature}</div>
                {meta.length > 0 && <div className="text-[9px] opacity-60">{meta}</div>}
              </button>
            );
          })}
        </div>
      ) : (
        STD_NAMESPACES.map((ns) => {
          const values = STD_VALUES_BY_NAMESPACE.get(ns) ?? [];
          const fns = STD_BY_NAMESPACE.get(ns) ?? [];
          const isOpen = openNs === ns;
          return (
            <div key={ns}>
              <button
                onClick={() => toggle(ns)}
                className={`w-full text-left text-[11px] font-mono border ${NS_BORDER[ns] ?? 'border-slate-700'} ${NS_TEXT[ns] ?? 'text-slate-300'} hover:bg-white/5 rounded px-2 py-1.5 transition-colors cursor-pointer flex justify-between items-center`}
              >
                <span>{ns}</span>
                <span className="opacity-50 text-[9px]">{isOpen ? '▲' : '▼'} {values.length + fns.length}</span>
              </button>
              {isOpen && (
                <div className="flex flex-col gap-0.5 mt-0.5 pl-2 border-l border-[#2d3148]">
                  {values.map((value) => (
                    <button
                      key={value.qualifiedName}
                      title={`${value.qualifiedName}: ${value.valueType}`}
                      onClick={() => addStdValue(value)}
                      className={`text-left text-[11px] font-mono ${NS_TEXT[ns] ?? 'text-slate-300'} hover:bg-white/10 rounded px-2 py-0.5 transition-colors cursor-pointer`}
                    >
                      {value.name}
                    </button>
                  ))}
                  {fns.map((fn) => (
                    <button
                      key={fn.signatureKey}
                      title={fn.displaySignature}
                      onClick={() => addStdFunc(fn)}
                      className={`text-left text-[11px] font-mono ${NS_TEXT[ns] ?? 'text-slate-300'} hover:bg-white/10 rounded px-2 py-0.5 transition-colors cursor-pointer`}
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

      <div className="border-t border-[#2d3148] my-1" />
      <div className="text-[10px] text-slate-500 uppercase tracking-wider">WASM</div>
      <button
        onClick={() => void runGraphWithWasm()}
        disabled={isRunningWasm}
        className="text-xs font-mono border border-indigo-700 text-indigo-300 hover:bg-indigo-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
      >
        {isRunningWasm ? 'Generating…' : 'Generate + Run Graph'}
      </button>
      <div className="text-[10px] text-slate-500 leading-4">
        Generated source and runtime output appear in the editor pane.
        {wasmStatus === 'running' ? ' Graph execution is in progress.' : ''}
      </div>

      <div className="border-t border-[#2d3148] my-1" />
      <button
        onClick={clearGraph}
        className="text-xs font-mono border border-red-800 text-red-400 hover:bg-red-950 rounded px-3 py-1.5 transition-colors cursor-pointer"
      >
        Clear
      </button>
      <button
        onClick={() => fileInputRef.current?.click()}
        className="text-xs font-mono border border-cyan-700 text-cyan-300 hover:bg-cyan-950 rounded px-3 py-1.5 transition-colors cursor-pointer"
      >
        Load .fleaux
      </button>
      <input
        ref={fileInputRef}
        type="file"
        accept=".fleaux,text/plain"
        className="hidden"
        onChange={(evt) => void handleLoadFile(evt)}
      />
    </aside>
  );
}
