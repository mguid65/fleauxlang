#!/usr/bin/env node

import { chromium } from '@playwright/test';
import { spawn, execFile as execFileCallback } from 'node:child_process';
import { mkdtempSync, cpSync, readdirSync, readFileSync, writeFileSync, rmSync, accessSync, constants } from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execFile = promisify(execFileCallback);

const thisFile = fileURLToPath(import.meta.url);
const scriptDir = dirname(thisFile);
const visualDir = resolve(scriptDir, '..');
const repoRoot = resolve(visualDir, '..');
const samplesDir = resolve(repoRoot, 'samples');

const defaultHost = '127.0.0.1';
const defaultPort = 4174;

const sampleArgProviders = new Map([
  ['25_fleaux_parser.fleaux', (samplePath) => [samplePath]],
]);

function parseArgs(argv) {
  const samples = [];
  let fleauxBin = '';
  let keepTemp = false;
  let port = defaultPort;

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === '--sample') {
      const sampleName = argv[index + 1];
      if (!sampleName) {
        throw new Error('Missing value for --sample');
      }
      samples.push(sampleName);
      index += 1;
      continue;
    }
    if (arg === '--fleaux-bin') {
      fleauxBin = argv[index + 1] ?? '';
      if (!fleauxBin) {
        throw new Error('Missing value for --fleaux-bin');
      }
      index += 1;
      continue;
    }
    if (arg === '--port') {
      const parsedPort = Number.parseInt(argv[index + 1] ?? '', 10);
      if (!Number.isFinite(parsedPort)) {
        throw new Error('Missing or invalid value for --port');
      }
      port = parsedPort;
      index += 1;
      continue;
    }
    if (arg === '--keep-temp') {
      keepTemp = true;
      continue;
    }

    throw new Error('Unknown argument: ' + arg);
  }

  return { samples, fleauxBin, keepTemp, port };
}

function findDefaultFleauxBinary() {
  const candidates = [
    resolve(repoRoot, 'core/cmake-build-debug/bin/fleaux'),
    resolve(repoRoot, 'core/cmake-build-relwithdebinfo/bin/fleaux'),
    resolve(repoRoot, 'core/cmake-build-debug-coverage/bin/fleaux'),
  ];

  for (const candidate of candidates) {
    try {
      accessSync(candidate, constants.X_OK);
      return candidate;
    } catch {
      continue;
    }
  }

  return '';
}

function collectSamples(requestedSamples) {
  const sampleNames = readdirSync(samplesDir)
    .filter((entry) => entry.endsWith('.fleaux'))
    .sort((lhs, rhs) => lhs.localeCompare(rhs));

  if (requestedSamples.length === 0) {
    return sampleNames;
  }

  for (const sampleName of requestedSamples) {
    if (!sampleNames.includes(sampleName)) {
      throw new Error('Requested sample not found: ' + sampleName);
    }
  }

  return requestedSamples;
}

function sampleRuntimeArgs(samplePath) {
  const provider = sampleArgProviders.get(samplePath.split('/').pop() ?? '');
  return provider ? provider(samplePath) : [];
}

async function runSample(fleauxBin, samplePath) {
  const forwardedArgs = sampleRuntimeArgs(samplePath);
  const args = [samplePath];
  if (forwardedArgs.length > 0) {
    args.push('--', ...forwardedArgs);
  }

  try {
    const result = await execFile(fleauxBin, args, {
      cwd: repoRoot,
      encoding: 'utf8',
      maxBuffer: 16 * 1024 * 1024,
    });
    return {
      exitCode: 0,
      stdout: result.stdout,
      stderr: result.stderr,
    };
  } catch (error) {
    return {
      exitCode: typeof error.code === 'number' ? error.code : 1,
      stdout: typeof error.stdout === 'string' ? error.stdout : '',
      stderr: typeof error.stderr === 'string' ? error.stderr : String(error),
    };
  }
}

function normalizeRunStream(text, baselinePath, roundtripPath, sampleName) {
  return text
    .split(baselinePath).join(sampleName)
    .split(roundtripPath).join(sampleName);
}

function normalizeRunResult(result, baselinePath, roundtripPath, sampleName) {
  return {
    exitCode: result.exitCode,
    stdout: normalizeRunStream(result.stdout, baselinePath, roundtripPath, sampleName),
    stderr: normalizeRunStream(result.stderr, baselinePath, roundtripPath, sampleName),
  };
}

async function waitForServer(url) {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    try {
      const statusCode = await new Promise((resolvePromise, rejectPromise) => {
        const parsedUrl = new URL(url);
        const transport = parsedUrl.protocol === 'https:' ? https : http;
        const request = transport.request(parsedUrl, { method: 'GET', agent: false }, (response) => {
          response.resume();
          resolvePromise(response.statusCode ?? 0);
        });
        request.on('error', rejectPromise);
        request.setTimeout(1_000, () => {
          request.destroy(new Error('Timed out waiting for dev server response'));
        });
        request.end();
      });

      if (statusCode >= 200 && statusCode < 500) {
        return;
      }
    } catch {
      // keep polling
    }
    await new Promise((resolvePromise) => setTimeout(resolvePromise, 250));
  }
  throw new Error('Timed out waiting for dev server: ' + url);
}

async function startDevServer(port) {
  const command = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  const child = spawn(command, ['run', 'dev', '--', '--host', defaultHost, '--port', String(port), '--strictPort'], {
    cwd: visualDir,
    env: { ...process.env },
    detached: process.platform !== 'win32',
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  child.stdout.on('data', (chunk) => {
    process.stdout.write(String(chunk));
  });
  child.stderr.on('data', (chunk) => {
    process.stderr.write(String(chunk));
  });

  const harnessUrl = 'http://' + defaultHost + ':' + String(port) + '/wasm-harness.html';
  await waitForServer(harnessUrl);
  return { child, harnessUrl };
}

async function resolvePort(requestedPort) {
  if (requestedPort !== defaultPort) {
    return requestedPort;
  }

  return await new Promise((resolvePromise, rejectPromise) => {
    const server = net.createServer();
    server.unref();
    server.on('error', rejectPromise);
    server.listen(0, defaultHost, () => {
      const address = server.address();
      if (!address || typeof address === 'string') {
        server.close(() => rejectPromise(new Error('Unable to allocate a free port for the round-trip runner')));
        return;
      }

      const port = address.port;
      server.close((closeError) => {
        if (closeError) {
          rejectPromise(closeError);
          return;
        }
        resolvePromise(port);
      });
    });
  });
}

function killDevServerProcessTree(child, signal) {
  if (process.platform === 'win32') {
    const command = signal === 'SIGKILL'
      ? 'taskkill /PID ' + String(child.pid) + ' /T /F'
      : 'taskkill /PID ' + String(child.pid) + ' /T';
    return execFile(process.env.ComSpec ?? 'cmd.exe', ['/d', '/s', '/c', command]).catch(() => undefined);
  }

  try {
    process.kill(-child.pid, signal);
  } catch {
    try {
      child.kill(signal);
    } catch {
      // ignore teardown races
    }
  }
  return Promise.resolve();
}

function stopDevServer(child) {
  return new Promise((resolvePromise) => {
    if (child.exitCode !== null || child.signalCode !== null) {
      resolvePromise();
      return;
    }

    let finished = false;
    const finish = () => {
      if (finished) {
        return;
      }
      finished = true;
      resolvePromise();
    };

    child.once('close', finish);
    child.once('exit', finish);

    void killDevServerProcessTree(child, 'SIGTERM');

    setTimeout(() => {
      if (finished) {
        return;
      }
      void killDevServerProcessTree(child, 'SIGKILL');
    }, 3_000).unref();
  });
}

async function main() {
  const { samples: requestedSamples, fleauxBin: requestedBin, keepTemp, port: requestedPort } = parseArgs(process.argv.slice(2));
  const fleauxBin = requestedBin || findDefaultFleauxBinary();
  if (!fleauxBin) {
    throw new Error('Unable to locate fleaux binary. Pass --fleaux-bin explicitly.');
  }
  const port = await resolvePort(requestedPort);

  const sampleNames = collectSamples(requestedSamples);
  const tempRoot = mkdtempSync(join(tmpdir(), 'fleaux-visual-roundtrip-'));
  const baselineSamplesDir = join(tempRoot, 'baseline');
  const roundtripSamplesDir = join(tempRoot, 'roundtrip');
  cpSync(samplesDir, baselineSamplesDir, { recursive: true });
  cpSync(samplesDir, roundtripSamplesDir, { recursive: true });

  const failures = [];
  let devServer = null;
  let browser = null;

  try {
    devServer = await startDevServer(port);
    browser = await chromium.launch({ headless: true });
    const page = await browser.newPage();
    await page.goto(devServer.harnessUrl);
    await page.waitForFunction(() => window.__fleauxWasmHarnessReady === true);

    for (const sampleName of sampleNames) {
      const baselinePath = join(baselineSamplesDir, sampleName);
      const roundtripPath = join(roundtripSamplesDir, sampleName);
      const sourceText = readFileSync(baselinePath, 'utf8');

      process.stdout.write('[roundtrip] ' + sampleName + '\n');

      const originalRun = await runSample(fleauxBin, baselinePath);

      let regenerated;
      try {
        regenerated = await page.evaluate(async ({ source }) => {
          const [{ importFleauxSourceToGraph }, { serializeGraphToFleaux }] = await Promise.all([
            import('/src/lib/fleauxToGraph.ts'),
            import('/src/lib/graphToFleaux.ts'),
          ]);
          const imported = importFleauxSourceToGraph(source);
          return serializeGraphToFleaux(imported.nodes, imported.edges);
        }, { source: sourceText });
      } catch (error) {
        failures.push({
          sampleName,
          stage: 'graph-roundtrip',
          detail: error instanceof Error ? error.message : String(error),
        });
        continue;
      }

      writeFileSync(roundtripPath, regenerated.sourceText, 'utf8');
      const regeneratedRun = await runSample(fleauxBin, roundtripPath);
      const normalizedOriginalRun = normalizeRunResult(originalRun, baselinePath, roundtripPath, sampleName);
      const normalizedRegeneratedRun = normalizeRunResult(regeneratedRun, baselinePath, roundtripPath, sampleName);

      if (
        normalizedOriginalRun.exitCode !== normalizedRegeneratedRun.exitCode ||
        normalizedOriginalRun.stdout !== normalizedRegeneratedRun.stdout ||
        normalizedOriginalRun.stderr !== normalizedRegeneratedRun.stderr
      ) {
        failures.push({
          sampleName,
          stage: 'execution-mismatch',
          detail: {
            originalRun: normalizedOriginalRun,
            regeneratedRun: normalizedRegeneratedRun,
            regeneratedSource: regenerated.sourceText,
          },
        });
        continue;
      }
    }
  } finally {
    if (browser) {
      await browser.close();
    }
    if (devServer) {
      await stopDevServer(devServer.child);
    }
    if (!keepTemp) {
      rmSync(tempRoot, { recursive: true, force: true });
    } else {
      process.stdout.write('Kept temp artifacts at ' + tempRoot + '\n');
    }
  }

  if (failures.length > 0) {
    await new Promise((resolvePromise) => process.stderr.write('\nRound-trip failures (' + String(failures.length) + '):\n', resolvePromise));
    for (const failure of failures) {
      await new Promise((resolvePromise) => process.stderr.write('[' + failure.stage + '] ' + failure.sampleName + '\n', resolvePromise));
      if (typeof failure.detail === 'string') {
        await new Promise((resolvePromise) => process.stderr.write(failure.detail + '\n', resolvePromise));
      } else {
        await new Promise((resolvePromise) => process.stderr.write(JSON.stringify(failure.detail, null, 2) + '\n', resolvePromise));
      }
      await new Promise((resolvePromise) => process.stderr.write('\n', resolvePromise));
    }
    return 1;
  }

  await new Promise((resolvePromise) => process.stdout.write('\nRound-trip succeeded for ' + String(sampleNames.length) + ' samples.\n', resolvePromise));
  return 0;
}

main()
  .then((exitCode) => {
    process.exit(exitCode);
  })
  .catch((error) => {
    process.stderr.write((error instanceof Error ? error.stack : String(error)) + '\n', () => {
      process.exit(1);
    });
  });




