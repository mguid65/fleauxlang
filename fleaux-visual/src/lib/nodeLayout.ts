import { callSlotCount } from './functionSignatures';

// The minimum height that fits a call node's argument slots.
export function callNodeMinHeight(params: { name: string; type: string }[], variadicSlots: number | undefined): number {
  return 80 + Math.max(0, callSlotCount(params, variadicSlots) - 1) * 20;
}
