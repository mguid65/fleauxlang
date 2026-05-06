import { type Node, type NodeProps } from '@xyflow/react';
import type { RawSourceData } from '../lib/types';

export default function RawSourceNode({ data }: NodeProps<Node<RawSourceData>>) {
  return (
    <div className="rounded-lg border border-amber-500 bg-amber-950 text-amber-100 shadow-lg min-w-70 max-w-105"
         style={{padding: '4px'}}
    >
      <div className="text-[10px] font-bold text-amber-400 mb-1">RAW SOURCE</div>
      <pre className="text-xs font-mono whitespace-pre-wrap wrap-break-word leading-5">{data.statementText + ';'}</pre>
    </div>
  );
}


