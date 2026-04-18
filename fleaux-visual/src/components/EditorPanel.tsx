import Editor from '@monaco-editor/react';
import type * as Monaco from 'monaco-editor';
import { useMemo } from 'react';
import { useFlowStore } from '../store/flowStore';
import { configureFleauxMonaco } from '../lib/monacoFleaux';

function beforeMount(monaco: typeof Monaco) {
  configureFleauxMonaco(monaco);
}

export function EditorPanel() {
  const sourceText = useFlowStore((s) => s.sourceText);
  const setSourceText = useFlowStore((s) => s.setSourceText);
  const loadGraphFromSource = useFlowStore((s) => s.loadGraphFromSource);
  const runEditorSourceWithWasm = useFlowStore((s) => s.runEditorSourceWithWasm);
  const wasmOutput = useFlowStore((s) => s.wasmOutput);
  const wasmMessage = useFlowStore((s) => s.wasmMessage);
  const wasmStatus = useFlowStore((s) => s.wasmStatus);
  const isRunning = wasmStatus === 'running';

  const statusBadgeClass = {
    idle: 'border-slate-700 text-slate-400',
    running: 'border-amber-700 text-amber-300',
    success: 'border-emerald-700 text-emerald-300',
    error: 'border-red-800 text-red-300',
  }[wasmStatus];

  const outputText = useMemo(() => {
    const sections: string[] = [];
    if (wasmStatus !== 'idle') {
      sections.push(`Status: ${wasmStatus.toUpperCase()}`);
    }
    if (wasmMessage) {
      sections.push(wasmMessage);
    }
    if (wasmOutput.trim().length > 0) {
      sections.push(wasmOutput);
    }
    return sections.join('\n\n');
  }, [wasmMessage, wasmOutput, wasmStatus]);

  const handleApplyToGraph = () => {
    try {
      loadGraphFromSource(sourceText);
    } catch {
      // Store state already carries the formatted import error for the output panel.
    }
  };

  return (
    <aside className="w-[min(48vw,760px)] min-w-105 max-w-[55vw] h-full border-l border-[#2d3148] bg-[#121420] flex flex-col shrink-0">
      <div className="px-4 py-3 border-b border-[#2d3148] flex items-center justify-between gap-3">
        <div>
          <div className="text-xs font-bold text-slate-400 uppercase tracking-widest">Fleaux Editor</div>
          <div className="text-[11px] text-slate-500 mt-1">
            Graph generation replaces the current editor contents, and you can edit, run, or re-import that code afterward.
          </div>
        </div>
        <div className="flex items-center gap-2 shrink-0 flex-wrap justify-end">
          <div className={`text-[10px] font-mono border rounded px-2 py-1 ${statusBadgeClass}`}>
            {wasmStatus.toUpperCase()}
          </div>
          <button
            type="button"
            onClick={handleApplyToGraph}
            disabled={isRunning || sourceText.trim().length === 0}
            className="text-xs font-mono border border-cyan-700 text-cyan-300 hover:bg-cyan-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
          >
            Apply to Graph
          </button>
          <button
            type="button"
            onClick={() => void runEditorSourceWithWasm()}
            disabled={isRunning || sourceText.trim().length === 0}
            className="text-xs font-mono border border-indigo-700 text-indigo-300 hover:bg-indigo-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
          >
            {isRunning ? 'Running…' : 'Run Editor'}
          </button>
        </div>
      </div>

      <div className="min-h-0 border-b border-[#2d3148]" style={{ flex: 3 }}>
        <Editor
          beforeMount={beforeMount}
          defaultLanguage="fleaux"
          language="fleaux"
          theme="fleaux-dark"
          value={sourceText}
          onChange={(value: string | undefined) => setSourceText(value ?? '')}
          options={{
            automaticLayout: true,
            minimap: { enabled: false },
            wordWrap: 'on',
            scrollBeyondLastLine: false,
            fontSize: 13,
            lineNumbersMinChars: 3,
            padding: { top: 12, bottom: 12 },
            bracketPairColorization: { enabled: true },
            guides: { bracketPairs: true },
          }}
        />
      </div>

      <section className="min-h-0 flex flex-col" style={{ flex: 2 }}>
        <div className="px-4 py-3 border-b border-[#2d3148]">
          <div className="text-xs font-bold text-slate-400 uppercase tracking-widest">Program Output</div>
          <div className="text-[11px] text-slate-500 mt-1">
            Runtime output and formatted diagnostics appear here.
          </div>
        </div>
        <div className="flex-1 overflow-auto p-4">
          <pre className="whitespace-pre-wrap wrap-break-word text-[12px] leading-5 font-mono text-slate-200 min-h-full">
            {outputText || 'No output yet. Run the graph to generate code and execute it.'}
          </pre>
        </div>
      </section>
    </aside>
  );
}


