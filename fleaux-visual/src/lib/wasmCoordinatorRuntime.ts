import { buildWasmCoordinatorError, WasmStatusCode } from './wasmCoordinatorCommon';
import type { WasmRunResult, WasmValidationResult } from './wasmCoordinatorCommon';

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
  mainScriptUrlOrBlob?: string | Blob;
  [key: string]: unknown;
};

type FleauxWasmFactory = (opts?: FleauxWasmOptions) => Promise<FleauxWasmModule>;

let modulePromise: Promise<FleauxWasmModule> | null = null;

function resolvePublicAssetUrl(path: string): string {
  const normalizedPath = path.replace(/^\/+/, '');
  const baseUrl = new URL(import.meta.env.BASE_URL, globalThis.location.href);
  return new URL(normalizedPath, baseUrl).toString();
}

async function loadFleauxWasmCoordinator(): Promise<FleauxWasmModule> {
  if (!modulePromise) {
    modulePromise = (async () => {
      const coordinatorUrl = resolvePublicAssetUrl('wasm/fleaux_wasm_coordinator.js');
      const mod = await import(/* @vite-ignore */ coordinatorUrl);
      const factory = (mod.default ?? mod) as FleauxWasmFactory;

      return factory({
        locateFile: (filename: string) => resolvePublicAssetUrl('wasm/' + filename),
        mainScriptUrlOrBlob: coordinatorUrl,
      });
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

export async function wasmCoordinatorVersionImpl(): Promise<string> {
  const wasm = await loadFleauxWasmCoordinator();
  return callString(wasm, 'fleaux_wasm_version');
}

export async function wasmParseAndLowerImpl(sourceText: string, sourceName = 'visual_graph.fleaux'): Promise<void> {
  const wasm = await loadFleauxWasmCoordinator();
  const rc = callWithSource(wasm, 'fleaux_wasm_parse_and_lower', sourceText, sourceName);
  throwIfFailed(wasm, 'parse and lower', rc);
}

export async function wasmValidateSourceImpl(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmValidationResult> {
  const wasm = await loadFleauxWasmCoordinator();
  const version = callString(wasm, 'fleaux_wasm_version');
  const rc = callWithSource(wasm, 'fleaux_wasm_parse_and_lower', sourceText, sourceName);
  throwIfFailed(wasm, 'validate', rc);
  return { version };
}

export async function wasmRunSourceImpl(
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
