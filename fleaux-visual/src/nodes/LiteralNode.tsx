import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import { useState } from 'react';
import type { LiteralData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

const VALUE_TYPE_COLORS: Record<LiteralData['valueType'], string> = {
  Float64: 'border-sky-500 bg-sky-950 text-sky-300',
  Int64: 'border-cyan-500 bg-cyan-950 text-cyan-300',
  UInt64: 'border-indigo-500 bg-indigo-950 text-indigo-300',
  Number: 'border-sky-500 bg-sky-950 text-sky-300',
  String: 'border-emerald-500 bg-emerald-950 text-emerald-300',
  Bool: 'border-amber-500 bg-amber-950 text-amber-300',
  Null: 'border-slate-500 bg-slate-900 text-slate-400',
};

const BADGE_COLORS: Record<LiteralData['valueType'], string> = {
  Float64: 'bg-sky-800 text-sky-200',
  Int64: 'bg-cyan-800 text-cyan-200',
  UInt64: 'bg-indigo-800 text-indigo-200',
  Number: 'bg-sky-800 text-sky-200',
  String: 'bg-emerald-800 text-emerald-200',
  Bool: 'bg-amber-800 text-amber-200',
  Null: 'bg-slate-700 text-slate-300',
};

function isNumericLiteralType(valueType: LiteralData['valueType']): boolean {
  return valueType === 'Float64' || valueType === 'Int64' || valueType === 'UInt64' || valueType === 'Number';
}

function formatLiteralLabel(valueType: LiteralData['valueType'], value: string): string {
  switch (valueType) {
    case 'String':
      return JSON.stringify(value);
    case 'Bool':
      return value.trim().toLowerCase() === 'false' ? 'False' : 'True';
    case 'Null':
      return 'null';
    default:
      return value.trim() || '0';
  }
}

function normalizeLiteralValue(valueType: LiteralData['valueType'], value: string): string {
  switch (valueType) {
    case 'Bool':
      return value.trim().toLowerCase() === 'false' ? 'False' : 'True';
    case 'Null':
      return 'null';
    default:
      return value;
  }
}

export function LiteralNode({ data, id }: NodeProps<Node<LiteralData>>) {
  const [isEditing, setIsEditing] = useState(false);
  const [editValue, setEditValue] = useState(data.value);
  const updateNodeData = useFlowStore((s) => s.updateNodeData);

  const colors = VALUE_TYPE_COLORS[data.valueType];
  const badge = BADGE_COLORS[data.valueType];
  const isBoolLiteral = data.valueType === 'Bool';
  const isNullLiteral = data.valueType === 'Null';

  const handleSave = () => {
    const normalizedValue = normalizeLiteralValue(data.valueType, editValue);
    const nextLabel = formatLiteralLabel(data.valueType, normalizedValue);

    if (normalizedValue !== data.value || nextLabel !== data.label) {
      updateNodeData(id, {
        value: normalizedValue,
        label: nextLabel,
      });
    }
    setEditValue(normalizedValue);
    setIsEditing(false);
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement | HTMLSelectElement>) => {
    if (e.key === 'Enter') handleSave();
    if (e.key === 'Escape') {
      setEditValue(data.value);
      setIsEditing(false);
    }
  };

  return (
    <div className={`rounded-lg border shadow-lg min-w-[120px] ${colors}`}
         style={{padding: '4px'}}
    >
      <span className={`text-[10px] font-bold rounded px-1.5 py-0.5 ${badge} mb-1 inline-block`}>
        {data.valueType}
      </span>
      {isEditing ? (
        isBoolLiteral ? (
          <select
            autoFocus
            value={normalizeLiteralValue('Bool', editValue)}
            onChange={(e) => setEditValue(e.target.value)}
            onBlur={handleSave}
            onKeyDown={handleKeyDown}
            className="w-full mt-1 px-2 py-1 text-sm font-mono bg-slate-900 border border-slate-600 rounded text-amber-200 focus:border-amber-400 outline-none"
          >
            <option value="True">True</option>
            <option value="False">False</option>
          </select>
        ) : (
          <input
            autoFocus
            type={isNumericLiteralType(data.valueType) ? 'number' : 'text'}
            value={editValue}
            onChange={(e) => setEditValue(e.target.value)}
            onBlur={handleSave}
            onKeyDown={handleKeyDown}
            className="w-full mt-1 px-2 py-1 text-sm font-mono bg-slate-900 border border-slate-600 rounded text-sky-200 focus:border-sky-400 outline-none"
          />
        )
      ) : (
        <div
          onClick={() => {
            if (isNullLiteral) {
              return;
            }
            setEditValue(normalizeLiteralValue(data.valueType, data.value));
            setIsEditing(true);
          }}
          className={`text-sm font-mono mt-1 truncate max-w-[200px] transition-opacity ${isNullLiteral ? 'cursor-default opacity-90' : 'cursor-text hover:opacity-80'}`}
          title={isNullLiteral ? 'Null literals are fixed' : 'Click to edit'}
        >
          {data.label}
        </div>
      )}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
