import {
  WasmCoordinatorError,
  wasmCoordinatorVersion,
  wasmRunSource,
  wasmValidateSource,
} from '../lib/wasmCoordinator';

type HarnessSuccess<T> = { ok: true; value: T };
type HarnessFailure = {
  ok: false;
  name: string;
  message: string;
  statusCode?: number;
  statusLabel?: string;
};
type HarnessResult<T> = HarnessSuccess<T> | HarnessFailure;

type WasmHarness = {
  version: () => Promise<HarnessResult<string>>;
  validate: (sourceText: string, sourceName?: string) => Promise<HarnessResult<{ version: string }>>;
  run: (sourceText: string, sourceName?: string) => Promise<HarnessResult<{ version: string; exitCode: number; output: string }>>;
};

declare global {
  interface Window {
    __fleauxWasmHarness?: WasmHarness;
    __fleauxWasmHarnessReady?: boolean;
  }
}

function toFailure(error: unknown): HarnessFailure {
  if (error instanceof WasmCoordinatorError) {
    return {
      ok: false,
      name: error.name,
      message: error.message,
      statusCode: error.statusCode,
      statusLabel: error.statusLabel,
    };
  }

  if (error instanceof Error) {
    return {
      ok: false,
      name: error.name,
      message: error.message,
    };
  }

  return {
    ok: false,
    name: 'UnknownError',
    message: String(error),
  };
}

async function capture<T>(fn: () => Promise<T>): Promise<HarnessResult<T>> {
  try {
    return { ok: true, value: await fn() };
  } catch (error) {
    return toFailure(error);
  }
}

const harness: WasmHarness = {
  version: () => capture(() => wasmCoordinatorVersion()),
  validate: (sourceText: string, sourceName = 'wasm_harness_validate.fleaux') =>
    capture(() => wasmValidateSource(sourceText, sourceName)),
  run: (sourceText: string, sourceName = 'wasm_harness_run.fleaux') =>
    capture(() => wasmRunSource(sourceText, sourceName)),
};

window.__fleauxWasmHarness = harness;
window.__fleauxWasmHarnessReady = true;

const status = document.querySelector<HTMLPreElement>('#status');
if (status) {
  status.textContent = 'ready';
}

