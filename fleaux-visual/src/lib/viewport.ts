import type { FitViewOptions, Node } from '@xyflow/react';
import type { FleauxNodeData } from './types';

// Fit View leaves room for the node toolbar on the left and the action bar along the top, which float over the canvas.
export const FIT_VIEW_OPTIONS: FitViewOptions<Node<FleauxNodeData>> = {
  padding: { top: '80px', left: '250px', right: '40px', bottom: '40px' },
  duration: 250,
};
