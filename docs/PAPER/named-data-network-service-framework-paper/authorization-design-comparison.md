# Authorization Design Comparison for NDNSF

## Manuscript Section

### Rationale for ABE-Backed Service Authorization

NDNSF separates authorization to participate in a service invocation from the User's runtime choice of Provider. Before selection, eligible Providers need a discovery descriptor, such as a service location, deadline, or resource requirement, to decide whether to return a positive ACK. This descriptor is readable by Providers holding the required ABE decryption capability and confidential against parties without that capability. It is not hidden from eligible but unselected Providers, and encrypting Content does not hide Data names or traffic patterns. Full application inputs require a separate, recipient-specific confidentiality boundary after selection.

Trust Schema and ABE enforce different aspects of this design. A Trust Schema relates Data names to acceptable signing keys and certificate chains. DNMP demonstrates how role signing keys constrain measurement commands and how VerSec selects a compatible key when constructing a command [1]. ABE instead restricts recovery of encrypted content keys. A signature-based authorization design can therefore retain the same ABE protection for discovery. The relevant comparison is whether NDNSF should also issue service-role signing credentials, or use its ABE capabilities, identity validation, and request-state checks for service authorization.

We consider two signature-based alternatives: an NDNSF adaptation of DNMP-style role authorization, and a design that issues a certificate for each authorized entity–role–service combination. These alternatives retain NDNSF's Request–ACK–Selection–Response workflow. DNMP itself is a published reference for role authorization, not an implementation of either NDNSF alternative. For a controlled design comparison, all alternatives must enforce the same service-entitlement matrix, authenticate participants, bind messages to the request and selected Provider, reject replay, and provide equivalent discovery and application-data confidentiality. Table 1 compares authorization credential organization under these shared requirements.

**Table 1. Qualitative comparison of authorization designs for NDNSF. The two certificate-based alternatives are proposed adaptations; their integration costs have not been measured.**

| Dimension | DNMP-inspired role-certificate authorization | Per-service role-certificate authorization | NDNSF ABE-backed authorization |
| --- | --- | --- | --- |
| Caller entitlement representation | Role credentials and rules relating roles to permitted services or commands | One credential for each authorized entity–role–service combination | Per-identity permission records and an aggregate KP-ABE DKEY |
| Heterogeneous service sets | Compose suitable roles; credential count depends on role reuse | Issue the required subset of service credentials | Encode the allowed service subset in the identity's DKEY policy |
| Signing credential selection | Select a compatible role key | Select the key for the service and message role | Retain identity signing credentials across services |
| Authorization decision | Validate role authority and permitted command, then request state | Validate entity, service, role, message type, and request state | Validate identity and permissions, check ABE-protected Challenges, then request state |
| Rule maintenance | Maintain role-to-action relations; shared rules can cover many users | Generic role/service matching can cover many certificates | Maintain service policies and common message/state validation |
| Adding a service entitlement | Issue a suitable credential or update the applicable role policy | Issue or enable the corresponding service credential | Replace the target identity's aggregate policy and refresh its DKEY |
| Removing an entitlement | Withdraw relevant credential authority; update any affected confidentiality keys separately | Withdraw the service credential; update any affected confidentiality keys separately | Current Controller withdrawal rotates its ABE generation and rebuilds effective policies |
| Granularity and isolation | Role and command scope can be narrowly defined | Separate service signing credentials support service-specific isolation | Current policy routing is service-level; one DKEY aggregates its holder's decryption rights |
| Confidentiality credentials | Additional encryption credentials are required | Additional encryption credentials are required | Provider DKEYs protect discovery; User permission DKEYs also support the ACK Challenge path |

The credential advantage of NDNSF is aggregation. Its Controller constructs an OR policy from the service permissions assigned to each identity. For example, a User authorized for Detection and Mapping receives a policy corresponding to `/PERMISSION/Detection OR /PERMISSION/Mapping`. Within one Attribute Authority and ABE parameter generation, a current DKEY can encode both entitlements while the identity-signing credential remains unchanged. KP-ABE supports this separation by placing the access policy in the decryption key and attributes on the encrypted content key [2]. Neither a separate signing certificate per service nor a new role for each service combination is required by this NDNSF design.

To characterize the scope of this advantage, let U be the number of authorized Users and E the number of User–service grants. With one active certificate per grant, the per-service alternative maintains E caller authorization certificates. An aggregate KP-ABE design maintains U current caller DKEYs under a single authority and parameter generation. A role-based alternative maintains the credentials required by its role assignment, which may be substantially fewer than E. These are counts of different authorization objects, excluding common identity/encryption credentials, authority material, content keys, and historical versions. They do not establish a storage or performance ordering. All designs must represent the entitlement relation, and the size and processing cost of an ABE policy key can increase with its complexity [2].

NDNSF also benefits from using one service-permission configuration to derive ABE policies and admission checks. When discovery already requires ABE, this avoids an additional service-role signing-credential lifecycle. The reuse is partial: protected Requests require Provider decryption credentials, whereas the authorization handshake additionally requires User permission DKEYs for ABE-protected ACK Challenges. Challenge processing and User DKEY distribution must therefore be counted as authorization costs. The Challenges provide evidence of access to the required decryption capability within the exchange; they do not replace signature validation, identity matching, or replay state, and do not prove the availability of claimed computing resources.

Lifecycle behavior further limits the comparison. The current Controller can grant an entitlement by updating the target identity's policy without rotating ABE public parameters. Withdrawal, however, rotates the Controller's ABE generation and rebuilds the effective policies, potentially requiring other authorized participants to refresh their key material. Certificate-based withdrawal can be more localized for execution authorization, although withdrawing access to shared encrypted content requires its own key-management mechanism. Both approaches require bounded credential/status freshness, and neither can erase plaintext or keys already disclosed to an authorized participant.

We select ABE-backed authorization for service-level entitlements because it supports confidential discovery to eligible Providers without enumerating them in each Request, aggregates heterogeneous service permissions, and retains identity-signing credentials across services. The certificate-based alternatives remain appropriate where independently scoped signing authority or finer command constraints justify their additional credentials. Our rationale establishes a fit between NDNSF's requirements and its credential organization; lower total storage, latency, or revocation overhead remains an empirical question.

## References

The reference numbers below are local to this section and must be merged with the manuscript bibliography when inserted.

1. Kathleen Nichols. “Lessons Learned Building a Secure Network Measurement Framework using Basic NDN.” ACM ICN, 2019. DOI: [10.1145/3357150.3357397](https://doi.org/10.1145/3357150.3357397). [Author PDF](https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf), particularly Sections 2.4 and 3.3.
2. Saurab Dulal, Tianyuan Yu, Siqi Liu, Adam Robert Thieme, Lixia Zhang, and Lan Wang. “Enhancing NAC-ABE to Support Access Control for mHealth Applications and Beyond.” arXiv:2311.07299, 2023. DOI: [10.48550/arXiv.2311.07299](https://doi.org/10.48550/arXiv.2311.07299). [PDF](https://arxiv.org/pdf/2311.07299), particularly Sections 3.2.3, 4.2, and 4.3.

## Editorial Evidence and Insertion Notes

以下内容用于作者核对，不属于待插入论文的英文正文。

- 本节完成的是文献与源码支撑的设计比较，不是实测开销比较，也不证明授权机制的新颖性。ARS 写作检查用于限定论据和简化表达；DNMP 的原始协议与假设的 NDNSF 集成已明确区分。
- 比较的共同前提是服务权限矩阵一致。不能以当前 service-level ABE 去宣称已经覆盖 DNMP 的所有参数级限制，也不能假设 Trust Schema 必须逐用户或逐服务手写一条规则。
- 一份 DKEY 表示一个聚合授权对象，不表示一段固定长度的秘密，更不表示所有 User 共用私钥。用户、Provider、签名、接收者加密、内容密钥及旧版本材料应分别计量。
- 论文正文应与最终投稿源码保持一致。这里引用的是下列当前 Controller 行为，不应作为旧投稿快照的能力说明。没有修改原论文正文、PDF 或 slides。

### Source Checkpoint

- Inspection baseline: `e8eafabc03b7b8ea4f106ecdae8e0de1fce18505` on `Experimental`.
- [ServiceController.cpp](../../../ndn-service-framework/ServiceController.cpp): `addAttributesForUsersAccordingToServicePolicy()` aggregates service attributes using OR; `grant()` replaces the target policy under unchanged ABE parameters; `revoke()` calls `rotateAbeGenerationAndReissuePolicies()`.
- `ServiceController.cpp` SHA-256: `c07f783fe2921a25beea46e92b5d2ff287a96213047a933742c866cddc287140`.
- [comparision.tex](comparision.tex), service-role CertificateV2 design: one `(entity, role, service)` credential, plus recipient-encryption credentials. This is a proposed alternative, not a measured runtime baseline.
- `comparision.tex` SHA-256: `c3a2c8ab6e0db146247a54c6b900e5b20f9795d1fb803e79bb8634be38e71b12`.

### Review and Validation Boundary

已逐项检查三类过强断言：角色数量不必随权限组合指数增长；Trust Schema 不天然要求每服务独立私钥；ABE 不提供与策略复杂度无关的固定成本。已补充 User permission DKEY 和 Challenge 的额外成本，保留 Controller 范围撤销轮换这一不利因素。参考文献标题、作者及 DOI 已核对，源码函数和已有服务证书设计已交叉核对；文档检查只覆盖链接、表格结构、源码摘要及 diff，不构成安全证明或实验资格。

Context Mode、CodeGraph 与 ARS 用于检索、源码核对和文献写作。此工作仅整理已接受的比较，不建立新协议/API/实验计划，因此不启动新的 Spec Kit 或 GSD 阶段，也不改变任何 DI 实现任务的验收状态。

下一步：合入论文时统一引用编号，并按最终投稿实现复核撤销描述；如需声称总体管理成本更低，应另行固定共同权限矩阵，分别测量凭证总字节数、密码学耗时、grant/revoke 影响身份数和交换消息量。
