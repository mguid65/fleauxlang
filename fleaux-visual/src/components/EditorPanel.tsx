import Editor from '@monaco-editor/react';
import type * as Monaco from 'monaco-editor';
import { useMemo, useState } from 'react';
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
  const [collapsed, setCollapsed] = useState(false);

  const statusBadgeClass = {
    idle: 'border-neutral-700 text-neutral-400',
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

  // A tab on the panel's left edge, in the same place whether the panel is open or collapsed.
  const collapseToggle = (
    <button
      type="button"
      onClick={() => setCollapsed(!collapsed)}
      title={collapsed ? 'Show the Fleaux editor' : 'Hide the Fleaux editor'}
      className="absolute top-1/2 -left-4 -translate-y-1/2 z-20 h-14 w-4 rounded-l border border-r-0 border-line bg-raised text-[10px] font-mono text-neutral-400 hover:text-neutral-100 hover:bg-hover flex items-center justify-center cursor-pointer"
    >
      {collapsed ? '<' : '>'}
    </button>
  );

  // `nokey`: React Flow listens for its shortcuts (Space to pan, Backspace to delete) on the whole window and only
  // skips <input>, <textarea>, and contenteditable targets. Monaco types into an EditContext <div>, which React Flow
  // does not recognize, so without this it cancels Space and can delete selected nodes on Backspace.
  if (collapsed) {
    return (
      <aside className="nokey relative w-9 h-full border-l border-line bg-panel flex flex-col items-center shrink-0">
        {collapseToggle}
        <div className="mt-1 text-[10px] font-bold text-neutral-500 uppercase tracking-widest [writing-mode:vertical-rl]">
          Fleaux Editor
        </div>
      </aside>
    );
  }

  return (
    <aside className="nokey relative w-[min(48vw,760px)] min-w-105 max-w-[55vw] h-full border-l border-line bg-panel flex flex-col shrink-0">
      {collapseToggle}
      <div className="px-4 py-3 border-b border-line flex items-center justify-between gap-3">
        <div className="text-xs font-bold text-neutral-400 uppercase tracking-widest">Fleaux Editor</div>
        <div className="flex items-center gap-2 shrink-0 flex-wrap justify-end">
          <div className={`text-[10px] font-mono border rounded px-2 py-1 ${statusBadgeClass}`}>
            {wasmStatus.toUpperCase()}
          </div>
          <button
            type="button"
            onClick={handleApplyToGraph}
            disabled={isRunning || sourceText.trim().length === 0}
            className="text-xs font-mono border border-emerald-700 text-emerald-300 hover:bg-emerald-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
          >
            Apply to Graph
          </button>
          <button
            type="button"
            onClick={() => void runEditorSourceWithWasm()}
            disabled={isRunning || sourceText.trim().length === 0}
            className="text-xs font-mono border border-amber-700 text-amber-300 hover:bg-amber-950 disabled:opacity-50 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer"
          >
            {isRunning ? 'Running…' : 'Run Editor'}
          </button>
        </div>
      </div>

      <div className="min-h-0 border-b border-line" style={{ flex: 3 }}>
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
        <div className="px-4 py-3 border-b border-line">
          <div className="text-xs font-bold text-neutral-400 uppercase tracking-widest">Program Output</div>
          <div className="text-[11px] text-neutral-500 mt-1">
            Runtime output and formatted diagnostics appear here.
          </div>
        </div>
        <div className="flex-1 overflow-auto p-4">
          <pre className="whitespace-pre-wrap wrap-break-word text-[12px] leading-5 font-mono text-neutral-200 min-h-full">
            {outputText || 'No output yet. Run the graph to generate code and execute it.'}
          </pre>
        </div>
      </section>
    </aside>
  );
}


