# Native Authority Configuration

## Status and Entry

R11-B1 / PARTIAL。独立 authority 的 C++ 入口为
`examples/DI_NativeArtifactAuthority.cpp`，Waf target 为 `DI_NativeArtifactAuthority`，
CLI 只接受 `--config FILE`。它复用 `NativeArtifactGrantIssuer` 的既有签名、policy、
recipient encryption 和 publication-source 校验，不创建平行 DI 协作协议。

```bash
DI_NativeArtifactAuthority --help
DI_NativeArtifactAuthority --config authority.json
```

`--run-for-ms` 是配置中的可选 bounded-process-test 控制项；省略或为零表示持续服务。
authority 进程启动后向 Controller 请求 ProviderPermission，再注册 TargetedOnly service。
真实 Controller、requester 和 Provider 的跨进程请求仍由 R11-B2/T016 验收。

## Configuration Schema

顶层 `schema` 必须为 `ndnsf-di-native-authority-v1`。配置文件中的相对路径相对该文件
所在目录解析；权限、目录隔离和 key 文件生命周期由部署者负责。

```json
{
  "schema": "ndnsf-di-native-authority-v1",
  "authority": {
    "identity": "/example/authority",
    "service": "/grant-authority",
    "group": "/example/group",
    "controller_identity": "/example/controller",
    "trust_schema_file": "policy/trust-schema.conf",
    "requester_identity": "/example/requester",
    "protection_epoch": "epoch-1",
    "content_key_id": "model-key-1",
    "authority_private_key_file": "keys/authority-signing.pem",
    "requester_public_key_file": "keys/requester-signing-public.pem",
    "content_key_file": "keys/model-content.key",
    "allowed_model_manifests": ["sha256:<64 lowercase hex characters>"],
    "recipient_public_key_files": {
      "/example/provider": "keys/provider-recipient-public.pem"
    }
  },
  "max_grant_ttl_ms": 60000,
  "permission_bootstrap_ms": 60000,
  "run_for_ms": 0
}
```

The `authority` object requires `identity`, `service`, `group`, `controller_identity`,
`trust_schema_file`, `requester_identity`, `protection_epoch`, `content_key_id`,
`authority_private_key_file`, `requester_public_key_file`, `content_key_file`,
`allowed_model_manifests`, and `recipient_public_key_files`. `publication_sources` is optional
when all requests use an already allowed manifest; when present it is immutable source metadata
indexed by an allowed package manifest and must contain `model_name`, `model_content_digest`,
`canonical_source_digest`, and `artifact_profile_digest`, plus an optional
`initializer_object_digest`.

`max_grant_ttl_ms` defaults to 60000 and must be between 1 and 3600000. A request expiry must
be in the future and no farther than this bound. `permission_bootstrap_ms` defaults to 60000,
must be between 1 and 3600000, and bounds the initial Controller ProviderPermission fetch;
the service does not announce readiness before that permission is installed. `run_for_ms` is
only a process-lifetime test bound and does not alter grant policy.

## Ownership and Transport Boundary

The authority process is the only owner of `authority_private_key_file`, `content_key_file`,
recipient public-key registry, and immutable artifact policy. The requester process must not
read, inherit, or accept these values. Its request envelope contains the signed requester fields,
the requested expiry, and the canonical published manifest JSON only:

- request schema: `ndnsf-di-native-grant-authority-request-v1`;
- response schema: `ndnsf-di-native-key-grant-v1`;
- transport: existing Core signed `RequestServiceTargeted`/`ResponseMessage` service transport;
- failure: `DI_PROTECTED_GRANT_REJECTED`, with no local issuer or plaintext fallback.

The authority handler checks the authenticated requester identity, configured requester public
key, protection epoch, expiry bound, manifest/source policy, recipient identity and request
signature before returning the existing signed and recipient-encrypted `NativeKeyGrant`. The
handler has no publication side effect. The requester verifies the returned authority signature,
grant binding and expiry before publishing through Core.

## Validation Ownership

R11-B1 proves schema canonicalization, authority key ownership, requester private-key exclusion,
the C++ authority executable and positive/negative grant component behavior. R11-B2 must prove
that the service is reached from an independent requester process and that a real grant crosses
the Core transport. A built executable, an in-process issuer test, or a Python wrapper check is
not process qualification.
