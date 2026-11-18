import { readdir } from 'node:fs/promises';
for (const name of (await readdir(new URL('.', import.meta.url))).filter(name => name.endsWith('.test.js')).sort()) {
  await import(new URL(name, import.meta.url));
}
