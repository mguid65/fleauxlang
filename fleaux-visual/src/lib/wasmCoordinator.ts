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

export interface WasmValidationResult {
  version: string;
}

export interface WasmRunResult {
  version: string;
  exitCode: number;
  output: string;
}

// The WASM coordinator lives in public/wasm/ — we load it via a blob URL so
// Vite's dev-server module interception (which appends ?import and can break
// Emscripten's import.meta.url path resolution) is completely bypassed.
let modulePromise: Promise<FleauxWasmModule> | null = null;

export async function loadFleauxWasmCoordinator(): Promise<FleauxWasmModule> {
  if (!modulePromise) {
    modulePromise = (async () => {
      const coordinatorUrl = '/wasm/fleaux_wasm_coordinator.js';

      const response = await fetch(coordinatorUrl);
      if (!response.ok) {
        throw new Error(`Failed to fetch WASM coordinator (${response.status}): ${coordinatorUrl}`);
      }
      const jsText = await response.text();

      // Create a transient blob URL so the browser can import it as an ES
      // module without Vite transforming or intercepting the request.
      const blob = new Blob([jsText], { type: 'application/javascript' });
      const blobUrl = URL.createObjectURL(blob);

      const mod = await import(/* @vite-ignore */ blobUrl);
      const factory = (mod.default ?? mod) as FleauxWasmFactory;

      // Pass locateFile so the WASM binary is always fetched from the known
      // public path, regardless of what import.meta.url resolved to inside
      // the blob module.
      const instance = await factory({
        locateFile: (filename: string) => `/wasm/${filename}`,
      });

      URL.revokeObjectURL(blobUrl);
      return instance;
    })();
  }
  return modulePromise;
}

export async function wasmCoordinatorVersion(): Promise<string> {
  const wasm = await loadFleauxWasmCoordinator();
  return wasm.ccall<string>('fleaux_wasm_version', 'string', [], []);
}

export async function wasmParseAndLower(sourceText: string, sourceName = 'visual_graph.fleaux'): Promise<void> {
  const wasm = await loadFleauxWasmCoordinator();
  const rc = wasm.ccall<number>(
    'fleaux_wasm_parse_and_lower',
    'number',
    ['string', 'string'],
    [sourceText, sourceName],
  );
  if (rc !== 0) {
    const detail = wasm.ccall<string>('fleaux_wasm_last_error', 'string', [], []);
    throw new Error(detail || `wasm coordinator failed with rc=${rc}`);
  }
}

export async function wasmValidateSource(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmValidationResult> {
  const version = await wasmCoordinatorVersion();
  await wasmParseAndLower(sourceText, sourceName);
  return { version };
}

export async function wasmRunSource(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmRunResult> {
  const wasm = await loadFleauxWasmCoordinator();
  const version = wasm.ccall<string>('fleaux_wasm_version', 'string', [], []);
  const rc = wasm.ccall<number>(
    'fleaux_wasm_run_source',
    'number',
    ['string', 'string'],
    [sourceText, sourceName],
  );

  if (rc !== 0) {
    const detail = wasm.ccall<string>('fleaux_wasm_last_error', 'string', [], []);
    throw new Error(detail || `wasm run failed with rc=${rc}`);
  }

  const output = wasm.ccall<string>('fleaux_wasm_last_output', 'string', [], []);
  const exitCode = wasm.ccall<number>('fleaux_wasm_last_exit_code', 'number', [], []);
  return {
    version,
    exitCode,
    output,
  };
}

