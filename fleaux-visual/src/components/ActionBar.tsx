import { useReactFlow } from '@xyflow/react';
import { useEffect, useRef, useState, type ChangeEvent } from 'react';
import { SAMPLES, type SampleEntry } from '../lib/samples';
import { FIT_VIEW_OPTIONS } from '../lib/viewport';
import { useFlowStore } from '../store/flowStore';

const FILE_BUTTON =
  'text-xs font-mono border border-emerald-700 text-emerald-300 hover:bg-emerald-950 rounded px-3 py-1.5 transition-colors cursor-pointer';
const EDIT_BUTTON =
  'text-xs font-mono border border-violet-700 text-violet-300 hover:bg-violet-950 disabled:opacity-40 disabled:cursor-not-allowed rounded px-3 py-1.5 transition-colors cursor-pointer';

function withFleauxExtension(name: string): string {
  return name.endsWith('.fleaux') ? name : `${name}.fleaux`;
}

function downloadText(fileName: string, text: string): void {
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = fileName;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function SamplesMenu({ onOpen }: { onOpen: (sample: SampleEntry) => void }) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const menuRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!open) {
      return;
    }
    const closeOnOutsideClick = (event: MouseEvent) => {
      if (!menuRef.current?.contains(event.target as Node)) {
        setOpen(false);
      }
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setOpen(false);
      }
    };
    document.addEventListener('mousedown', closeOnOutsideClick);
    document.addEventListener('keydown', closeOnEscape);
    return () => {
      document.removeEventListener('mousedown', closeOnOutsideClick);
      document.removeEventListener('keydown', closeOnEscape);
    };
  }, [open]);

  const lowerQuery = query.toLowerCase();
  const shown = SAMPLES.filter((sample) => sample.title.toLowerCase().includes(lowerQuery));

  return (
    <div ref={menuRef} className="relative">
      <button type="button" onClick={() => setOpen(!open)} className={FILE_BUTTON}>
        Samples {open ? '▲' : '▼'}
      </button>
      {open && (
        <div className="absolute top-full left-0 mt-1 w-72 max-h-[60vh] flex flex-col bg-raised border border-emerald-800 rounded-lg shadow-xl z-20">
          <input
            autoFocus
            type="text"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="Filter samples…"
            className="m-2 px-2 py-1 text-xs font-mono bg-emerald-950 border border-emerald-800 rounded text-emerald-100 placeholder-emerald-700 outline-none focus:border-emerald-500"
          />
          <div className="overflow-auto px-1 pb-1">
            {shown.map((sample) => (
              <button
                key={sample.fileName}
                type="button"
                title={sample.fileName}
                onClick={() => {
                  setOpen(false);
                  onOpen(sample);
                }}
                className="w-full text-left text-[11px] font-mono text-emerald-200 hover:bg-emerald-950 rounded px-2 py-1 cursor-pointer"
              >
                {sample.title}
              </button>
            ))}
            {shown.length === 0 && <div className="px-2 py-1 text-[11px] font-mono text-emerald-700">No samples match.</div>}
          </div>
        </div>
      )}
    </div>
  );
}

// File, history, and layout actions, floating along the top of the canvas.
export function ActionBar() {
  const documentName = useFlowStore((s) => s.documentName);
  const canUndo = useFlowStore((s) => s.past.length > 0);
  const canRedo = useFlowStore((s) => s.future.length > 0);
  const undo = useFlowStore((s) => s.undo);
  const redo = useFlowStore((s) => s.redo);
  const tidyLayout = useFlowStore((s) => s.tidyLayout);
  const clearGraph = useFlowStore((s) => s.clearGraph);
  const loadGraphFromSource = useFlowStore((s) => s.loadGraphFromSource);
  const graphSource = useFlowStore((s) => s.graphSource);
  const reactFlow = useReactFlow();
  const fileInputRef = useRef<HTMLInputElement | null>(null);

  // The store reports a failed import in the output panel, so the error needs no further handling here.
  const openSource = (source: string, name: string) => {
    try {
      loadGraphFromSource(source, name);
    } catch {
      // Already reported.
    }
  };

  const openSample = async (sample: SampleEntry) => {
    openSource(await sample.load(), sample.fileName);
  };

  const openFile = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (file) {
      openSource(await file.text(), file.name);
    }
  };

  const save = () => {
    const source = graphSource();
    if (source !== null) {
      downloadText(withFleauxExtension(documentName || 'graph'), source);
    }
  };

  const tidy = () => {
    tidyLayout();
    requestAnimationFrame(() => void reactFlow.fitView(FIT_VIEW_OPTIONS));
  };

  return (
    <div className="absolute top-4 left-60 right-4 z-10 flex pointer-events-none">
      <div className="pointer-events-auto flex flex-wrap items-center gap-2 bg-raised border border-line rounded-xl shadow-xl p-1.5">
        <SamplesMenu onOpen={(sample) => void openSample(sample)} />
        <button type="button" onClick={() => fileInputRef.current?.click()} className={FILE_BUTTON}>
          Load .fleaux
        </button>
        <button type="button" onClick={save} className={FILE_BUTTON}>
          Save .fleaux
        </button>
        <input
          ref={fileInputRef}
          type="file"
          accept=".fleaux,text/plain"
          className="hidden"
          onChange={(event) => void openFile(event)}
        />
        <span className="text-[11px] font-mono text-emerald-200 px-1" title="The graph saves as this file">
          {withFleauxExtension(documentName || 'graph')}
        </span>

        <div className="w-px self-stretch bg-line" />
        <button type="button" onClick={undo} disabled={!canUndo} title="Undo (Ctrl+Z)" className={EDIT_BUTTON}>
          Undo
        </button>
        <button type="button" onClick={redo} disabled={!canRedo} title="Redo (Ctrl+Shift+Z or Ctrl+Y)" className={EDIT_BUTTON}>
          Redo
        </button>
        <button type="button" onClick={tidy} title="Lay the graph out again" className={EDIT_BUTTON}>
          Tidy
        </button>

        <div className="w-px self-stretch bg-line" />
        <button
          type="button"
          onClick={clearGraph}
          className="text-xs font-mono border border-red-800 text-red-400 hover:bg-red-950 rounded px-3 py-1.5 transition-colors cursor-pointer"
        >
          Clear
        </button>
      </div>
    </div>
  );
}
