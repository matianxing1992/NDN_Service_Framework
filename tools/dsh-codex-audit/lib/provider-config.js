import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import TOML from '@iarna/toml';
import { AuditError } from './audit-manager.js';

const ID = /^[a-zA-Z0-9_-]+$/;
const ENV_NAME = /^[a-zA-Z_][a-zA-Z0-9_]*$/;
const STRINGS = ['name', 'base_url', 'env_key', 'wire_api'];
const NUMBERS = ['request_max_retries', 'stream_max_retries', 'stream_idle_timeout_ms'];
const FIELDS = new Set([...STRINGS, ...NUMBERS, 'env_http_headers', 'env_key_instructions',
  'requires_openai_auth', 'supports_websockets']);
const unsupported = message => new AuditError('CODEX_PROVIDER_CONFIG', message,
  '检查 DSH 宿主的 CODEX_HOME/config.toml。自定义 provider 使用 Responses API 和 env_key 环境变量认证。');
const table = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const override = (key, value) => ['-c', `${key}=${TOML.stringify.value(value)}`];

export async function readProviderConfig(home, env = process.env) {
  let config;
  try {
    config = TOML.parse(await readFile(join(home, 'config.toml'), 'utf8'));
  } catch (error) {
    if (error.code === 'ENOENT') return { id: 'openai', args: [] };
    throw unsupported('无法读取或解析 Codex 宿主配置。');
  }
  let scope;
  if (config.profile !== undefined) {
    scope = typeof config.profile === 'string' && config.profiles &&
      Object.hasOwn(config.profiles, config.profile) ? config.profiles[config.profile] : undefined;
    if (!table(scope)) throw unsupported('Codex 指定的默认 profile 没有有效的配置。');
  }
  const id = scope?.model_provider ?? config.model_provider ?? 'openai';
  if (typeof id !== 'string' || !ID.test(id)) throw unsupported('当前 Codex provider ID 无效。');
  if (id === 'openai') {
    if ((scope?.openai_base_url ?? config.openai_base_url) !== undefined ||
        (scope?.chatgpt_base_url ?? config.chatgpt_base_url) !== undefined) {
      throw unsupported('内置 OpenAI 路由设置了自定义地址。请把网关配置为独立 provider。');
    }
    return { id, args: [] };
  }
  const provider = config.model_providers && Object.hasOwn(config.model_providers, id)
    ? config.model_providers[id] : undefined;
  if (!table(provider)) throw unsupported('当前 Codex provider 缺少对应的 model_providers 配置表。');
  const fields = Object.keys(provider);
  if (fields.some(key => !FIELDS.has(key))) {
    throw unsupported('当前 Codex provider 使用独立审计尚不支持的字段，可能包含静态凭据或认证命令。');
  }
  if (STRINGS.some(key => Object.hasOwn(provider, key) && typeof provider[key] !== 'string') ||
      NUMBERS.some(key => Object.hasOwn(provider, key) &&
        (!Number.isSafeInteger(provider[key]) || provider[key] < 0)) ||
      (provider.supports_websockets !== undefined && typeof provider.supports_websockets !== 'boolean') ||
      (provider.wire_api !== undefined && provider.wire_api !== 'responses')) {
    throw unsupported('当前 Codex provider 的连接字段无效，或协议不是 Responses。');
  }
  if (provider.requires_openai_auth !== undefined && provider.requires_openai_auth !== false) {
    throw unsupported('自定义 provider 请求内置 OpenAI 登录凭据。请为它配置自己的 env_key。');
  }
  const url = URL.parse(provider.base_url ?? '');
  if (!url || !['https:', 'http:'].includes(url.protocol) ||
      url.username || url.password || url.search || url.hash) {
    throw unsupported('自定义 provider 需要明确的 HTTP(S) base_url，且地址不能包含凭据、查询参数或片段。');
  }
  const headers = provider.env_http_headers;
  if ((provider.env_key !== undefined && !ENV_NAME.test(provider.env_key)) ||
      (headers !== undefined && (!table(headers) || Object.entries(headers).some(([header, name]) =>
        !/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(header) || typeof name !== 'string' || !ENV_NAME.test(name))))) {
    throw unsupported('自定义 provider 的认证或请求头环境变量名无效。');
  }
  if (provider.env_key && !env[provider.env_key]?.trim()) {
    throw new AuditError('CODEX_PROVIDER_ENV_MISSING', `DSH 进程缺少 ${provider.env_key}。`,
      '把该变量配置到启动 DSH 的服务或终端环境，重启 DSH 后再检查。');
  }
  const selected = Object.fromEntries(fields.filter(key => key !== 'env_key_instructions')
    .map(key => [key, provider[key]]));
  const args = [...override('model_provider', id), ...override('model_providers', { [id]: selected })];
  const names = [...new Set([provider.env_key, ...Object.values(headers ?? {})].filter(Boolean))];
  if (names.length) args.push(...override('shell_environment_policy.exclude', names));
  return { id, args };
}
