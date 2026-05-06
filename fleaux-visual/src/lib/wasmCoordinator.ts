import {
  WasmCoordinatorError,
  WasmStatusCode,
} from './wasmCoordinatorCommon';
import type { WasmRunResult, WasmValidationResult } from './wasmCoordinatorCommon';
import {
  wasmCoordinatorVersionImpl,
  wasmParseAndLowerImpl,
  wasmRunSourceImpl,
  wasmValidateSourceImpl,
} from './wasmCoordinatorRuntime';

type WorkerRequestPayload =
  | { kind: 'version' }
  | { kind: 'parseAndLower'; sourceText: string; sourceName?: string }
  | { kind: 'validate'; sourceText: string; sourceName?: string }
  | { kind: 'run'; sourceText: string; sourceName?: string };

type WorkerRequest = WorkerRequestPayload & { id: number };

type WorkerResponse =
  | { id: number; ok: true; value: unknown }
  | {
      id: number;
      ok: false;
      error: {
        name: string;
        message: string;
        statusCode?: number;
        statusLabel?: string;
      };
    };

type PendingRequest = {
  resolve: (value: unknown) => void;
  reject: (reason?: unknown) => void;
};

const WORKER_REQUEST_TIMEOUT_MS = 12_000;

let workerPromise: Promise<Worker> | null = null;
let nextRequestId = 1;
let preferDirectRuntime = shouldPreferDirectRuntimeByDefault();
const pendingRequests = new Map<number, PendingRequest>();

function shouldPreferDirectRuntimeByDefault(): boolean {
  if (typeof navigator === 'undefined') {
    return false;
  }

  return /\bFirefox\//.test(navigator.userAgent);
}

function ensureThreadedWasmPrerequisites(): void {
  if (typeof globalThis.crossOriginIsolated === 'boolean' && globalThis.crossOriginIsolated === false) {
    throw new Error(
      'Fleaux threaded WASM requires a cross-origin isolated page. Ensure Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp are enabled, or load the GitHub Pages build once so the cross-origin isolation service worker can register and reload the page.',
    );
  }
}

function shouldFallbackToDirectRuntime(error: unknown): boolean {
  if (error instanceof WasmCoordinatorError || !(error instanceof Error)) {
    return false;
  }

  const message = error.message;
  return (
    message.includes('DataCloneError')
    || message.includes('loading-workers')
    || message.includes('bootstrap failure')
    || message.includes('unreadable message')
    || message.includes('timed out waiting for the WASM coordinator worker')
    || message.includes('WASM coordinator worker failed')
    || message.includes('Failed to construct')
  );
}

function toWorkerError(error: WorkerResponse & { ok: false }): Error {
  if (typeof error.error.statusCode === 'number' && typeof error.error.statusLabel === 'string') {
    return new WasmCoordinatorError(error.error.message, error.error.statusCode, error.error.statusLabel);
  }
  return new Error(error.error.message);
}

function rejectAllPending(error: unknown): void {
  for (const { reject } of pendingRequests.values()) {
    reject(error);
  }
  pendingRequests.clear();
}

function resetWorker(error: unknown): void {
  if (workerPromise) {
    workerPromise
      .then((worker) => worker.terminate())
      .catch(() => undefined);
  }
  workerPromise = null;
  rejectAllPending(error);
}

async function getWorker(): Promise<Worker> {
  if (!workerPromise) {
    workerPromise = Promise.resolve(
      new Worker(new URL('./wasmCoordinator.worker.ts', import.meta.url), { type: 'module' }),
    );

    const worker = await workerPromise;
    worker.onmessage = (event: MessageEvent<WorkerResponse>) => {
      const response = event.data;
      const pending = pendingRequests.get(response.id);
      if (!pending) {
        return;
      }

      pendingRequests.delete(response.id);
      if (response.ok) {
        pending.resolve(response.value);
        return;
      }

      pending.reject(toWorkerError(response));
    };

    worker.onerror = (event: ErrorEvent) => {
      event.preventDefault();
      resetWorker(new Error(event.message || 'WASM coordinator worker failed'));
    };

    worker.onmessageerror = () => {
      resetWorker(new Error('WASM coordinator worker produced an unreadable message'));
    };
  }

  return workerPromise;
}

async function callWorker<T>(request: WorkerRequestPayload): Promise<T> {
  ensureThreadedWasmPrerequisites();

  const worker = await getWorker();
  const id = nextRequestId++;

  return await new Promise<T>((resolve, reject) => {
    const timeoutId = globalThis.setTimeout(() => {
      resetWorker(new Error('Fleaux timed out waiting for the WASM coordinator worker to become ready.'));
    }, WORKER_REQUEST_TIMEOUT_MS);

    pendingRequests.set(id, {
      resolve: (value) => {
        globalThis.clearTimeout(timeoutId);
        resolve(value as T);
      },
      reject: (reason) => {
        globalThis.clearTimeout(timeoutId);
        reject(reason);
      },
    });

    worker.postMessage({ ...request, id } satisfies WorkerRequest);
  });
}

async function callCoordinator<T>(
  request: WorkerRequestPayload,
  directCall: () => Promise<T>,
): Promise<T> {
  ensureThreadedWasmPrerequisites();

  if (preferDirectRuntime) {
    return await directCall();
  }

  try {
    return await callWorker<T>(request);
  } catch (error) {
    if (!shouldFallbackToDirectRuntime(error)) {
      throw error;
    }

    preferDirectRuntime = true;
    resetWorker(error);
    return await directCall();
  }
}

export { WasmCoordinatorError, WasmStatusCode };
export type { WasmValidationResult, WasmRunResult };

export async function wasmCoordinatorVersion(): Promise<string> {
  return await callCoordinator<string>({ kind: 'version' }, () => wasmCoordinatorVersionImpl());
}

export async function wasmParseAndLower(sourceText: string, sourceName = 'visual_graph.fleaux'): Promise<void> {
  await callCoordinator<null>(
    { kind: 'parseAndLower', sourceText, sourceName },
    async () => {
      await wasmParseAndLowerImpl(sourceText, sourceName);
      return null;
    },
  );
}

export async function wasmValidateSource(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmValidationResult> {
  return await callCoordinator<WasmValidationResult>(
    { kind: 'validate', sourceText, sourceName },
    () => wasmValidateSourceImpl(sourceText, sourceName),
  );
}

export async function wasmRunSource(
  sourceText: string,
  sourceName = 'visual_graph.fleaux',
): Promise<WasmRunResult> {
  return await callCoordinator<WasmRunResult>(
    { kind: 'run', sourceText, sourceName },
    () => wasmRunSourceImpl(sourceText, sourceName),
  );
}

