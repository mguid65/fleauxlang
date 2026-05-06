import { Handle, Position } from '@xyflow/react';

const hiddenHandleStyle = {
  opacity: 0,
  pointerEvents: 'none' as const,
};

export function BodyRootMarkerHandles() {
  return (
    <>
      <Handle
        type="target"
        position={Position.Top}
        id="let-body-root"
        style={{ ...hiddenHandleStyle, left: '42%' }}
      />
      <Handle
        type="target"
        position={Position.Top}
        id="closure-body-root"
        style={{ ...hiddenHandleStyle, left: '58%' }}
      />
    </>
  );
}
