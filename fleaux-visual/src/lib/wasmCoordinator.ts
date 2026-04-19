type FleauxWasmModule = {
  ccall: <T = number | string>(
    ident: string,
    returnType: 'number' | 'string' | null,
    argTypes: Array<'string' | 'number'>,
    args: Array<string | number>,
  ) => T;
};

type FleauxWasmOptions = {
  locateFile?: (path: string, scriptDirectory: string) => string;
  [key: string]: unknown;
};

type FleauxWasmFactory = (opts?: FleauxWasmOptions) => Promise<FleauxWasmModule>;

// Status codes must match WasmStatus in fleaux_wasm_coordinator.cpp.
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

// The WASM coordinator lives in public/wasm/ — we load it via a blob URL so
// Vite's dev-server module interception (which appends ?import and can break
// Emscripten's import.meta.url path resolution) is completely bypassed.
//
// We do NOT cache rejected module loads. A transient fetch/import failure should
// still allow later retries to re-attempt the load.
let modulePromise: Promise<FleauxWasmModule> | null = null;

function buildWasmCoordinatorError(action: string, rc: number, detail: string): WasmCoordinatorError {
  const label = WASM_STATUS_LABELS[rc] ?? 'unknown_error';
  const message = detail
    ? `${action} failed (${label}, rc=${rc}): ${detail}`
    : `${action} failed (${label}, rc=${rc})`;
  return new WasmCoordinatorError(message, rc, label);
}

function resolvePublicAssetUrl(path: string): string {
  const normalizedPath = path.replace(/^\/+/, '');
  return new URL(normalizedPath, window.location.origin + import.meta.env.BASE_URL).toString();
}

export async function loadFleauxWasmCoordinator(): Promise<FleauxWasmModule> {
  if (!modulePromise) {
    modulePromise = (async () => {
      const coordinatorUrl = resolvePublicAssetUrl('wasm/fleaux_wasm_coordinator.js');

      const response = await fetch(coordinatorUrl);
      if (!response.ok) {
        throw new Error(`Failed to fetch WASM coordinator (${response.status}): ${coordinatorUrl}`);
      }
      const jsText = await response.text();

      // Create a transient blob URL so the browser can import it as an ES
      // module without Vite transforming or intercepting the request.
      const blob = new Blob([jsText], { type: 'application/javascript' });
      const blobUrl = URL.createObjectURL(blob);

      let instance: FleauxWasmModule;
      try {
        const mod = await import(/* @vite-ignore */ blobUrl);
        const factory = (mod.default ?? mod) as FleauxWasmFactory;

        // Pass locateFile so the WASM binary is always fetched from the known
        // public path, regardless of what import.meta.url resolved to inside
        // the blob module.
        instance = await factory({
          locateFile: (filename: string) => resolvePublicAssetUrl(`wasm/${filename}`),
        });
      } finally {
        URL.revokeObjectURL(blobUrl);
      }

      return instance;
    })().catch((error: unknown) => {
      modulePromise = null;
      throw error;
    });
  }
  return modulePromise;
}

function callString(wasm: FleauxWasmModule, ident: string): string {
  return wasm.ccall<string>(ident, 'string', [], []);
}

function callNumber(wasm: FleauxWasmModule, ident: string): number {
  return wasm.ccall<number>(ident, 'number', [], []);
}

function callWithSource(
  wasm: FleauxWasmModule,
  ident: string,
  sourceText: string,
  sourceName: string,
): number {
  return wasm.ccall<number>(ident, 'number', ['string', 'string'], [sourceText, sourceName]);
}

function readLastError(wasm: FleauxWasmModule): string {
  return callString(wasm, 'fleaux_wasm_last_error');
}

function throwIfFailed(wasm: FleauxWasmModule, action: string, rc: number): void {
  if (rc !== WasmStatusCode.Ok) {
    throw buildWasmCoordinatorError(action, rc, readLastError(wasm));
  }
}

export async function wasmCoordinatorVersion(): Promise<string> {
  const wasm = await loadFleauxWasmCoordinator();
  return callString(wasm, 'fleaux_wasm_version');
}

export async function wasmParseAndLower(sourceText: string, sourceName = 'visual_graph.fleaux'): Promise<void> {
  const wasm = await loadFleauxWasmCoordinator();
  const rc = callWithSource(wasm, 'fleaux_wasm_parse_and_lower', sourceText, sourceName);
  throwIfFailed(wasm, 'parse and lower', rc);
}

export async function wasmValidateSource(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmValidationResult> {
  const wasm = await loadFleauxWasmCoordinator();
  const version = callString(wasm, 'fleaux_wasm_version');
  const rc = callWithSource(wasm, 'fleaux_wasm_parse_and_lower', sourceText, sourceName);
  throwIfFailed(wasm, 'validate', rc);
  return { version };
}

export async function wasmRunSource(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmRunResult> {
  const wasm = await loadFleauxWasmCoordinator();
  const version = callString(wasm, 'fleaux_wasm_version');
  const rc = callWithSource(wasm, 'fleaux_wasm_run_source', sourceText, sourceName);
  throwIfFailed(wasm, 'run', rc);

  const output = callString(wasm, 'fleaux_wasm_last_output');
  const exitCode = callNumber(wasm, 'fleaux_wasm_last_exit_code');
  return { version, exitCode, output };
}

