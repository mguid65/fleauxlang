import { expect, test, type Page } from '@playwright/test';
type HarnessSuccess<T> = { ok: true; value: T };
type HarnessFailure = {
  ok: false;
  name: string;
  message: string;
  statusCode?: number;
  statusLabel?: string;
};
type HarnessResult<T> = HarnessSuccess<T> | HarnessFailure;
type WasmHarnessApi = {
  version: () => Promise<HarnessResult<string>>;
  validate: (sourceText: string, sourceName?: string) => Promise<HarnessResult<{ version: string }>>;
  run: (
    sourceText: string,
    sourceName?: string,
  ) => Promise<HarnessResult<{ version: string; exitCode: number; output: string }>>;
};
declare global {
  interface Window {
    __fleauxWasmHarness?: WasmHarnessApi;
    __fleauxWasmHarnessReady?: boolean;
  }
}
async function waitForHarness(page: Page) {
  await page.goto('/wasm-harness.html');
  await expect(page.locator('#status')).toHaveText('ready');
}
test('loads the coordinator assets and reports a version', async ({ page }) => {
  const assetUrls: string[] = [];
  page.on('response', (response) => {
    const url = response.url();
    if (url.includes('fleaux_wasm_coordinator.js') || url.includes('fleaux_wasm_coordinator.wasm')) {
      assetUrls.push(url);
    }
  });
  await waitForHarness(page);
  const result = await page.evaluate(async () => window.__fleauxWasmHarness?.version());
  expect(result).toBeDefined();
  expect(result?.ok).toBe(true);
  if (result?.ok) {
    expect(result.value).toContain('fleaux-wasm-coordinator/');
  }
  expect(assetUrls.some((url) => url.includes('fleaux_wasm_coordinator.js'))).toBe(true);
  expect(assetUrls.some((url) => url.includes('fleaux_wasm_coordinator.wasm'))).toBe(true);
});
test('reports an explicit empty-source validation error', async ({ page }) => {
  await waitForHarness(page);
  const result = await page.evaluate(async () => window.__fleauxWasmHarness?.validate('   \n\n   '));
  expect(result).toBeDefined();
  expect(result?.ok).toBe(false);
  if (result && !result.ok) {
    expect(result.statusLabel).toBe('empty_source');
    expect(result.message).toContain('empty source text');
  }
});
test('runs a file round-trip inside the browser virtual filesystem', async ({ page }) => {
  await waitForHarness(page);
  const source = [
    'import Std;',
    '("PWD", "/workspace/demo") -> Std.OS.SetEnv;',
    '("note.txt", "hello from wasm") -> Std.File.WriteText;',
    '("note.txt") -> Std.File.ReadText -> Std.Println;',
    '() -> Std.OS.Cwd -> Std.Println;',
    '',
  ].join('\n');
  const result = await page.evaluate(async ({ sourceText }) => window.__fleauxWasmHarness?.run(sourceText), {
    sourceText: source,
  });
  expect(result).toBeDefined();
  expect(result?.ok).toBe(true);
  if (result?.ok) {
    expect(result.value.exitCode).toBe(0);
    expect(result.value.output).toContain('hello from wasm');
    expect(result.value.output).toContain('/workspace/demo');
  }
});

test('resolves Std.Path.Absolute against virtual PWD after Std.OS.SetEnv', async ({ page }) => {
  await waitForHarness(page);
  const source = [
    'import Std;',
    '("PWD", "/workspace/demo") -> Std.OS.SetEnv;',
    '("note.txt") -> Std.Path.Absolute -> Std.Println;',
    '',
  ].join('\n');

  const result = await page.evaluate(async ({ sourceText }) => window.__fleauxWasmHarness?.run(sourceText), {
    sourceText: source,
  });

  expect(result).toBeDefined();
  expect(result?.ok).toBe(true);
  if (result?.ok) {
    expect(result.value.exitCode).toBe(0);
    expect(result.value.output).toContain('/workspace/demo/note.txt');
  }
});

test('resets virtual cwd to /workspace after Std.OS.UnsetEnv("PWD")', async ({ page }) => {
  await waitForHarness(page);
  const source = [
    'import Std;',
    '("PWD", "/workspace/demo") -> Std.OS.SetEnv;',
    '() -> Std.OS.Cwd -> Std.Println;',
    '("PWD") -> Std.OS.UnsetEnv -> Std.Println;',
    '() -> Std.OS.Cwd -> Std.Println;',
    '("note.txt") -> Std.Path.Absolute -> Std.Println;',
    '',
  ].join('\n');

  const result = await page.evaluate(async ({ sourceText }) => window.__fleauxWasmHarness?.run(sourceText), {
    sourceText: source,
  });

  expect(result).toBeDefined();
  expect(result?.ok).toBe(true);
  if (result?.ok) {
    expect(result.value.exitCode).toBe(0);
    expect(result.value.output).toContain('/workspace/demo');
    expect(result.value.output).toContain('True');
    expect(result.value.output).toContain('/workspace\n');
    expect(result.value.output).toContain('/workspace/note.txt');
  }
});

test('returns a clear unsupported-result payload for Std.OS.Exec on web', async ({ page }) => {
  await waitForHarness(page);
  const source = [
    'import Std;',
    '("echo hi") -> Std.OS.Exec -> Std.Println;',
    '',
  ].join('\n');
  const result = await page.evaluate(async ({ sourceText }) => window.__fleauxWasmHarness?.run(sourceText), {
    sourceText: source,
  });
  expect(result).toBeDefined();
  expect(result?.ok).toBe(true);
  if (result?.ok) {
    expect(result.value.output).toContain('Std.OS.Exec is unavailable on web/WASM targets');
  }
});
