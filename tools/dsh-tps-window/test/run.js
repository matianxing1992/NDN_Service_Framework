import { readdir } from 'node:fs/promises'
for (const file of (await readdir(new URL('.', import.meta.url))).filter(file => file.endsWith('.test.js')).sort()) {
  await import(new URL(file, import.meta.url))
}
