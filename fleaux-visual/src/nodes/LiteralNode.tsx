import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import { useState } from 'react';
import type { LiteralData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

const VALUE_TYPE_COLORS: Record<LiteralData['valueType'], string> = {
  Float64: 'border-orange-500 bg-orange-950 text-orange-300',
  Int64: 'border-green-500 bg-green-950 text-green-300',
  UInt64: 'border-yellow-500 bg-yellow-950 text-yellow-300',
  String: 'border-emerald-500 bg-emerald-950 text-emerald-300',
  Bool: 'border-amber-500 bg-amber-950 text-amber-300',
  Null: 'border-pink-500 bg-pink-950 text-pink-300',
};

// The value field while a literal is edited.
const EDIT_COLORS: Record<LiteralData['valueType'], string> = {
  Float64: 'border-orange-700 bg-orange-950 text-orange-100 focus:border-orange-400',
  Int64: 'border-green-700 bg-green-950 text-green-100 focus:border-green-400',
  UInt64: 'border-yellow-700 bg-yellow-950 text-yellow-100 focus:border-yellow-400',
  String: 'border-emerald-700 bg-emerald-950 text-emerald-100 focus:border-emerald-400',
  Bool: 'border-amber-700 bg-amber-950 text-amber-100 focus:border-amber-400',
  Null: 'border-pink-700 bg-pink-950 text-pink-100 focus:border-pink-400',
};

const BADGE_COLORS: Record<LiteralData['valueType'], string> = {
  Float64: 'bg-orange-800 text-orange-200',
  Int64: 'bg-green-800 text-green-200',
  UInt64: 'bg-yellow-800 text-yellow-200',
  String: 'bg-emerald-800 text-emerald-200',
  Bool: 'bg-amber-800 text-amber-200',
  Null: 'bg-pink-800 text-pink-200',
};

function isNumericLiteralType(valueType: LiteralData['valueType']): boolean {
  return valueType === 'Float64' || valueType === 'Int64' || valueType === 'UInt64';
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
  const editColors = EDIT_COLORS[data.valueType];
  const isBoolLiteral = data.valueType === 'Bool';
  const isNullLiteral = data.valueType === 'Null';
  const isStringLiteral = data.valueType === 'String';
  const editLineCount = editValue.split('\n').length;

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

  // Enter saves. In a string, Shift+Enter starts a new line.
  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement | HTMLSelectElement | HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !(isStringLiteral && e.shiftKey)) {
      e.preventDefault();
      handleSave();
    }
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
            className={`w-full mt-1 px-2 py-1 text-sm font-mono border rounded outline-none ${editColors}`}
          >
            <option value="True">True</option>
            <option value="False">False</option>
          </select>
        ) : isStringLiteral ? (
          // A text input would drop the line breaks of a multi-line string.
          <textarea
            autoFocus
            rows={Math.min(editLineCount, 12)}
            value={editValue}
            onChange={(e) => setEditValue(e.target.value)}
            onBlur={handleSave}
            onKeyDown={handleKeyDown}
            spellCheck={false}
            wrap="off"
            className={`nodrag nowheel block w-full mt-1 px-2 py-1 text-sm font-mono border rounded outline-none resize ${editLineCount > 1 ? 'min-w-[360px]' : ''} ${editColors}`}
          />
        ) : (
          <input
            autoFocus
            type={isNumericLiteralType(data.valueType) ? 'number' : 'text'}
            value={editValue}
            onChange={(e) => setEditValue(e.target.value)}
            onBlur={handleSave}
            onKeyDown={handleKeyDown}
            className={`w-full mt-1 px-2 py-1 text-sm font-mono border rounded outline-none ${editColors}`}
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
