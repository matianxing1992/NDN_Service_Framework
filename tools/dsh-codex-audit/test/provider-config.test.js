import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { readProviderConfig } from '../lib/provider-config.js';

test('provider preflight resolves only the selected profile and never exports credential values', async () => {
  const root = await mkdtemp(join(tmpdir(), 'dsh-codex-provider-'));
  const config = join(root, 'config.toml');
  try {
    await writeFile(config, 'model_provider = "openai"\n[profiles.unused]\nmodel_provider = "gateway"\n');
    assert.deepEqual(await readProviderConfig(root, {}), { id: 'openai', args: [] });
    const gateway = `model_provider = "gateway"
[model_providers.gateway]
base_url = "https://gateway.example/v1"
env_key = "GATEWAY_API_KEY"
env_http_headers = { X-Account = "GATEWAY_ACCOUNT" }
`;
    await writeFile(config, gateway);
    await assert.rejects(readProviderConfig(root, {}), { code: 'CODEX_PROVIDER_ENV_MISSING' });
    const result = await readProviderConfig(root, { GATEWAY_API_KEY: 'fixture-credential' });
    assert.equal(result.id, 'gateway');
    assert.ok(result.args.includes('model_provider="gateway"'));
    assert.ok(result.args.some(arg => arg.startsWith('shell_environment_policy.exclude=') &&
      arg.includes('GATEWAY_API_KEY') && arg.includes('GATEWAY_ACCOUNT')));
    assert.ok(!JSON.stringify(result).includes('fixture-credential'));
    for (const extra of ['experimental_bearer_token = "fixture-credential"', 'requires_openai_auth = true',
      'auth = { command = "never-run-this" }', 'wire_api = "chat"']) {
      await writeFile(config, gateway + extra + '\n');
      await assert.rejects(readProviderConfig(root, { GATEWAY_API_KEY: 'fixture-credential' }),
        error => error.code === 'CODEX_PROVIDER_CONFIG' && !error.message.includes('fixture-credential'));
    }
    await writeFile(config, '[broken = "fixture-credential"');
    await assert.rejects(readProviderConfig(root, {}), error => error.code === 'CODEX_PROVIDER_CONFIG' &&
      !error.message.includes('fixture-credential'));
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
