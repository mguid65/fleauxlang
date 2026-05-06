import { WasmCoordinatorError } from './wasmCoordinatorCommon';
import {
  wasmCoordinatorVersionImpl,
  wasmParseAndLowerImpl,
  wasmRunSourceImpl,
  wasmValidateSourceImpl,
} from './wasmCoordinatorRuntime';

type WorkerRequest =
  | { id: number; kind: 'version' }
  | { id: number; kind: 'parseAndLower'; sourceText: string; sourceName?: string }
  | { id: number; kind: 'validate'; sourceText: string; sourceName?: string }
  | { id: number; kind: 'run'; sourceText: string; sourceName?: string };

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

type WorkerFailureResponse = Extract<WorkerResponse, { ok: false }>;

function serializeError(error: unknown): WorkerFailureResponse['error'] {
  if (error instanceof WasmCoordinatorError) {
    return {
      name: error.name,
      message: error.message,
      statusCode: error.statusCode,
      statusLabel: error.statusLabel,
    };
  }

  if (error instanceof Error) {
    return {
      name: error.name,
      message: error.message,
    };
  }

  return {
    name: 'UnknownError',
    message: String(error),
  };
}

self.onmessage = async (event: MessageEvent<WorkerRequest>) => {
  const request = event.data;
  const requestId = request.id;

  try {
    switch (request.kind) {
      case 'version':
        self.postMessage({ id: request.id, ok: true, value: await wasmCoordinatorVersionImpl() } satisfies WorkerResponse);
        return;
      case 'parseAndLower':
        await wasmParseAndLowerImpl(request.sourceText, request.sourceName);
        self.postMessage({ id: request.id, ok: true, value: null } satisfies WorkerResponse);
        return;
      case 'validate':
        self.postMessage({ id: request.id, ok: true, value: await wasmValidateSourceImpl(request.sourceText, request.sourceName) } satisfies WorkerResponse);
        return;
      case 'run':
        self.postMessage({ id: request.id, ok: true, value: await wasmRunSourceImpl(request.sourceText, request.sourceName) } satisfies WorkerResponse);
        return;
      default: {
        self.postMessage({
          id: requestId,
          ok: false,
          error: { name: 'UnsupportedRequest', message: 'Unsupported wasm worker request' },
        } satisfies WorkerResponse);
        return;
      }
    }
  } catch (error) {
    self.postMessage({ id: request.id, ok: false, error: serializeError(error) } satisfies WorkerResponse);
  }
};



