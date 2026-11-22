// The repository's samples/*.fleaux files, bundled so that the editor can open them. Each one loads on demand.
const SAMPLE_LOADERS = import.meta.glob<string>('../../../samples/*.fleaux', { query: '?raw', import: 'default' });

export interface SampleEntry {
  fileName: string;
  // `01_hello_world.fleaux` -> `01 hello world`
  title: string;
  load: () => Promise<string>;
}

export const SAMPLES: SampleEntry[] = Object.entries(SAMPLE_LOADERS)
  .map(([path, load]) => {
    const fileName = path.slice(path.lastIndexOf('/') + 1);
    return { fileName, title: fileName.replace(/\.fleaux$/, '').replaceAll('_', ' '), load };
  })
  .sort((lhs, rhs) => lhs.fileName.localeCompare(rhs.fileName, undefined, { numeric: true }));
