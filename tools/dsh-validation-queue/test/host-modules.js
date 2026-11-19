import { registerHooks, createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

// Optional local-host verification without copying dependencies into this source checkout.
if (process.env.DSH_TEST_ANCHOR) {
  const require = createRequire(process.env.DSH_TEST_ANCHOR);
  registerHooks({ resolve(specifier, context, nextResolve) {
    try { return nextResolve(specifier, context); }
    catch (error) {
      if (!specifier.startsWith('@deepseek-ai/') && specifier !== 'zod') throw error;
      return { url: pathToFileURL(require.resolve(specifier)).href, shortCircuit: true };
    }
  } });
}
