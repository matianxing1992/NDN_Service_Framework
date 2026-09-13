# Source Build Handoff

## Scope And Status

**Status**: COMPLETE / SOURCE_READY。本交付固定NDNSF、NAC-ABE、NDN-SVS及传递依赖NDNSD源码，提供可迁移的构建输入及共享skills。

2026-09-06用户明确纠正范围：本机本轮只负责交付；后续编译、unit/integration、Python扩展验证、MiniNDN、SIF和Tiger测试全部移交另一台机器。此前自行扩大的本机构建已停止，保留原始记录；它们不再是交付完成的前置条件，也不能被记为完整验证PASS。Spec182仍不开始。

## Design And Ownership

沿用`adapters/slurm-apptainer/scripts/build-local-sif.sh`作为唯一SIF构建入口。
不复制第二条构建流程；新工具只准备/验证输入并渲染现有多阶段definition。

| File / symbol | Change / contract |
| --- | --- |
| `prepare-local-sif-source.py` / `NAC_ABE_FILES`, `NDNSD_FILES`, `main`, `seal_dependency` | ADD可选`--nac-abe-workspace`、`--ndnsd-workspace`生成`nacAbe.tar`、`ndnSd.tar`；`generated`参数仅用于显式派生的归档内容，保留旧默认。 |
| `prepare-local-sif-source.py` / `svs_version_bytes`, `selected_files` | ADD `--derive-ndn-svs-version`从固定wscript的VERSION/GIT_TAG_PREFIX和git describe导出`VERSION.info`，不读写旧生成物；归档同时保留legacy与canonical路径，使host preflight可以按实际workload路径核对。 |
| `validate-local-sif-source.py` / `validate_archive` | MODIFY支持相对于seal目录的归档路径；仍拒绝指向其他文件的路径，逐成员/大小/hash检查不变。 |
| `prepare-development-handoff.py` / `prepare`, `REQUIRED_WHEELS` | ADD接受四库checkout、锁文件、wheel目录及新输出目录；要求精确commit、无tracked修改，拒绝归档未跟踪源码；要求模板所用五个wheel齐全；复用既有sealer生成四个source-only归档、相对路径seal、输入manifest和definition模板。 |
| `prepare-development-handoff.py` / `verify` | ADD验证锁/每项文件hash、三库commit与source seal、归档内容；不调用编译器、Apptainer或SSH，不把SOURCE_READY当作运行PASS。 |
| `prepare-development-handoff.py` / `render` | ADD验证输入及外部base SIF摘要后，只替换模板中的包目录/base路径/seal/release身份，生成本机可执行definition；搬运后重新render，不编辑旧seal身份。 |
| `development-runtime.def.in` | ADD从历史r119多阶段recipe整理：新NAC/SVS/NDNSD/Core和两扩展在builder内重建；显式依赖prefix、完整输出集合、清除自有旧输出、builder/final摘要相等。base只提供基础依赖。 |
| `development-handoff.lock.json` | ADD四库GitHub Experimental来源及完整commit、base SIF身份、wheel文件名/hash/公开下载URL和获取要求；不含个人路径、模型/密钥或宿主二进制。 |
| `skills/` | ADD近期维护的可共享技能及引用；构建细节仍在Tiger目录，不在技能中另写一套脚本。 |

包目录由调用者独占；不覆盖已有非空目录。锁文件是输入身份，manifest是生成文件清单，seal负责原始源码字节；三个角色不互相替代。render的绝对路径仅存在于生成的definition，Git中的模板不含本机路径。

## Validation Plan

先静态核对接口/路径/依赖和模板，再做必要单测：依赖归档及篡改拒绝、源码未提交/错误commit拒绝、包搬迁后验证、错误wheel/base摘要拒绝、definition残留占位符/宿主二进制拒绝。对真实四库生成一份交付包并验证；不把模板静态门当作容器编译PASS。

既有构建入口要求`--host-gate-manifest`。交付不生成伪造的host资格清单，旧r119的结果不能证明新三库版本。正式构建/运行仍须满足现有输入门和容器ABI检查；需要改变实验范围时，由实验owner明确更新其运行契约。

## Source Pins And External Inputs

唯一机器可读锁为 [development-handoff.lock.json](../development-handoff.lock.json)。NDNSF runtime source固定`447f7584072142edea8c8e2ae3ed4ffdc5fbd0c5`；后续Experimental提交增加本交付工具、模板、技能与记录，不宣称改变该runtime源码。

| Repository | Revision | Purpose |
| --- | --- | --- |
| NDNSF | `447f7584072142edea8c8e2ae3ed4ffdc5fbd0c5` | 完整合并生产基线 |
| NAC-ABE | `5ed23e68520fb9c4ef747c51d6d18c915ff88fc4` | 合并授权修复及正确安装元数据 |
| NDN-SVS | `9f2d8a47cd2a25a5f9ade661c9dbe8acd6416a20` | 保留本机改动并正常merge远端；取消订阅/V2 mapping修复 |
| NDNSD | `375a35c5706d5a6b6f176cd98b72d63dcb7f5b10` | SVS的ABI消费者；修正pkg-config include后重编译 |

SVS SONAME保持`0.1.0`但类布局变化，必须重编译NDNSD、Core及两个扩展；不能只替换同名库。SVS离线版本为`0.1.0+git.9f2d8a47`，从固定源码派生，不复制本机旧`VERSION.info`。

Git携带工具、definition模板、锁和skills。四个源码tar与五个wheel在接收端由锁重建；也可直接搬运整包，搬运后先verify再render。base SIF不入Git，固定SHA-256为`b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`，记录的集群位置在锁文件；本轮未登录集群确认。APT仍需要联网，不宣称完整离线APT复现。模型、tokenizer、adapter、oracle与各角色身份按实验契约另行准备。

## Receiving Machine

**Current build policy (2026-09-12)**：接收机器所有新SIF构建统一使用Apptainer **1.5.3**，先核对实际binary及版本，构建命令传 `--expected-apptainer 1.5.3`。下文历史交付锁、base SIF及记录保持原身份，不从旧登录节点版本推导新构建版本。完整版本及验证规则见[SIF build](sif-build.md#apptainer-version-policy)。

保留另一台机器已有工作目录；在新目录获取NDNSF的`Experimental`。先阅读本文件和 [shared skills](../../../skills/README.md)，可直接要求代理读取 `skills/itiger-ndnsf-ops/SKILL.md`。根目录skills不会自动覆盖个人版本。

```bash
git clone --branch Experimental --single-branch \
  https://github.com/matianxing1992/NDN_Service_Framework.git ndnsf-experiment-delivery
cd ndnsf-experiment-delivery
```

从新的NDNSF checkout根执行以下准备命令。`../source-handoff-20260906`必须尚不存在；路径不要含空格。四库均采用锁中的完整commit，普通clone后detach，不覆盖现有开发分支。

```bash
python3 - <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

repo = Path.cwd()
lock = json.loads((repo / 'Experiments/TigerCluster/development-handoff.lock.json').read_text())
delivery = repo.parent / 'source-handoff-20260906'
delivery.mkdir()
(delivery / 'sources').mkdir()
(delivery / 'wheels').mkdir()
for name, row in lock['repositories'].items():
    target = delivery / 'sources' / name
    subprocess.run(['git', 'clone', '--branch', row['branch'], '--single-branch',
                    '--no-checkout', row['url'], str(target)], check=True)
    subprocess.run(['git', '-C', str(target), 'checkout', '--detach', row['revision']], check=True)
for row in lock['wheels']:
    with urllib.request.urlopen(row['url'], timeout=60) as response:
        content = response.read()
    if 'sha256:' + hashlib.sha256(content).hexdigest() != row['sha256']:
        raise SystemExit('WHEEL_DIGEST_MISMATCH:' + row['filename'])
    with (delivery / 'wheels' / row['filename']).open('xb') as output:
        output.write(content)
print(delivery)
PY

python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py prepare \
  --lock Experiments/TigerCluster/development-handoff.lock.json \
  --ndnsf-workspace ../source-handoff-20260906/sources/ndnsf \
  --nac-abe-workspace ../source-handoff-20260906/sources/nacAbe \
  --ndn-svs-workspace ../source-handoff-20260906/sources/ndnSvs \
  --ndnsd-workspace ../source-handoff-20260906/sources/ndnSd \
  --wheels ../source-handoff-20260906/wheels \
  --output ../source-handoff-20260906/bundle

python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py verify \
  --bundle ../source-handoff-20260906/bundle

python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py render \
  --bundle ../source-handoff-20260906/bundle \
  --base-sif /absolute/path/to/spec180-runtime.sif \
  --output ../source-handoff-20260906/runtime.def
```

最后一步替换base路径；工具会先核对实际SIF摘要。生成的definition在包外，包搬迁后重新render到新文件。获得当前源码所需的host qualification manifest和已核对版本的Apptainer后，按 [SIF build](sif-build.md) 调用既有`build-local-sif.sh`，传入新definition和`bundle/source/source-seal.json`。缺少该manifest时交付状态仍为SOURCE_READY，尚不具备该构建入口要求的全部前置证据；不能传入过期G3清单绕过。现有Tiny/YOLO/Qwen workload和Tiger profile各自的输入/验收边界继续有效。

构建成功后先保留新SIF摘要、build record、容器native manifest及实际loaded libraries证据，再运行有限Tiger功能用例。反馈必须含四库commit、SIF摘要、profile/config、日志和复现步骤；不要在开发机开始Spec182前回写实验PASS。

## Checkpoint

- [x] D001 Dependency source checkpoints and ABI handoff
- [x] D002 Portable source inputs and definition
- [x] D003 Shared skills and receiving-machine instructions
- [x] D004 Validated local package and GitHub Experimental publication

D001按用户明确范围修订为“固定源码并移交ABI重建要求”，不再要求本机完成兼容性编译/运行测试。四库Experimental已发布：NDNSF首次交付为`0a8d8cc7`，后续为进度/范围记录；NAC、SVS、NDNSD与锁文件一致。

已完成的交付检查：相关工具28/28 PASS（17.90s）、五项skill结构检查、28个文档相对链接及接收命令语法检查；真实四库包生成、搬迁后verify、固定base摘要及definition render PASS。这些是已发生的检查，不是下一轮测试指令。

本地包：`.codex-tmp/source-handoff-20260906/bundle-r3`，搬迁副本`relocated-r3`；NDNSF453、NAC63、SVS31、NDNSD14个源码成员。seal为`sha256:9129d07298f5754823f3bc2bf9c10fea7416adbb1e4168ced3dc82750948612c`。记录在同级`package-r3.log`、`relocation-r3.log`、`render-r3.log`、`tool-checks-r5/output.log`；这些本地产物不入Git，接收端可按上文重建。

额外启动的本机构建已按用户要求停止：R1执行器超时、R2重复构建中断、R3普通Provider构建成功、R4故障Provider构建被用户范围纠正中止。原始记录保留在独立树`ndnsf-svs-abi-20260906/.codex-tmp/svs-abi-20260906-r1/`，不声称完整构建或运行验证通过。

**TRANSFERRED**：另一台机器按锁定源码重新构建全部ABI消费者（含NDNSD及两个扩展），执行unit/integration、必要MiniNDN、SIF构建和Tiger验收。本机停止补测；SIF/新闭包运行资格均未取得。既有构建入口所需host qualification manifest仍是接收方需处理的前置项。

Spec182保持0/17；Tiger B003与历史Local R8的未完成/FAIL状态保留。下一步由实验机器按Receiving Machine步骤接收和验证。
