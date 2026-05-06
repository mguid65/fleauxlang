export const WasmStatusCode = {
  Ok: 0,
  EmptySource: 2,
  ParseError: 3,
  LowerError: 4,
  CompileError: 5,
  RuntimeError: 6,
  RuntimeUnavailable: 7,
} as const;

export type WasmStatusCode = (typeof WasmStatusCode)[keyof typeof WasmStatusCode];

const WASM_STATUS_LABELS: Record<number, string> = {
  [WasmStatusCode.Ok]: 'ok',
  [WasmStatusCode.EmptySource]: 'empty_source',
  [WasmStatusCode.ParseError]: 'parse_error',
  [WasmStatusCode.LowerError]: 'lower_error',
  [WasmStatusCode.CompileError]: 'compile_error',
  [WasmStatusCode.RuntimeError]: 'runtime_error',
  [WasmStatusCode.RuntimeUnavailable]: 'runtime_unavailable',
};

export interface WasmValidationResult {
  version: string;
}

export interface WasmRunResult {
  version: string;
  exitCode: number;
  output: string;
}

export class WasmCoordinatorError extends Error {
  readonly statusCode: number;
  readonly statusLabel: string;

  constructor(message: string, statusCode: number, statusLabel: string) {
    super(message);
    this.name = 'WasmCoordinatorError';
    this.statusCode = statusCode;
    this.statusLabel = statusLabel;
  }
}

export function buildWasmCoordinatorError(action: string, rc: number, detail: string): WasmCoordinatorError {
  const label = WASM_STATUS_LABELS[rc] ?? 'unknown_error';
  const message = detail
    ? action + ' failed (' + label + ', rc=' + String(rc) + '): ' + detail
    : action + ' failed (' + label + ', rc=' + String(rc) + ')';
  return new WasmCoordinatorError(message, rc, label);
}
