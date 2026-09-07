# Source Build Handoff

## Scope And Status

**Status**: IN_PROGRESS / SOURCE_DELIVERY_ONLY。本交付固定NDNSF、NAC-ABE、NDN-SVS及传递依赖NDNSD源码，整理可迁移的构建输入及共享skills；不执行SIF构建、Tiger作业或Spec182实现。实验验收仍由实验机器负责。

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

2026-09-06 TOOLING_CHECKPOINT：最终统一R5 **28/28 PASS，17.90s**（四文件：`test_development_handoff.py`、`test_prepare_local_sif_source.py`、`test_build_local_sif_record.py`、`test_development_runtime_template.py`）；raw `.codex-tmp/source-handoff-20260906/tool-checks-r5/output.log`。其中旧build-record fixture修复R3为8/8 PASS；扩展NDNSD前统一R4为26/26 PASS。五项skill frontmatter检查PASS，14份Markdown的28个相对链接及接收命令Python语法PASS。

真实包 `.codex-tmp/source-handoff-20260906/bundle-r3`：NDNSF453、NAC63、SVS31、NDNSD14个源码成员，四个归档总计12,072,960字节；加五个wheel、锁和模板。复制至`relocated-r3`后verify PASS，实际固定base摘要校验及definition render PASS，raw同级`package-r3.log`、`relocation-r3.log`、`render-r3.log`。seal为`sha256:9129d07298f5754823f3bc2bf9c10fea7416adbb1e4168ced3dc82750948612c`，静态定义检查含4库/9个原生产物。SIF_BUILD/CONTAINER_RUNTIME仍NOT_RUN。

SVS源码检查87/87 cases、639 assertions PASS；NDNSD pkg-config路径真实RED/GREEN PASS。新依赖下的NDNSF消费者fresh build/unit/integration尚在运行；不能将此前759/154结果当作本闭包结果。

消费者build R1在299/318触发执行器1800秒上限（exit124，1800.081s），首边界为`TIMEOUT_AT_RUNNER_BOUNDARY`，没有compiler error；R2在同一fresh build目录按相同配置/-j2续编，执行器有限上限3600秒。R1原始记录在独立树 `ndnsf-svs-abi-20260906/.codex-tmp/svs-abi-20260906-r1/build-r1/`，不覆盖。

R2日志随后证明Waf未保存R1 task signatures、实际重新编译全图，故SIGINT停止重复R2（exit68，78.516s）。R3只选择尚未完成的`di-native-provider`及必要依赖；R1已有Core/unit/integration/应用按同一源码和配置保留逐目标证据。任何中断轮次均不记PASS，最终须补齐交付目标并验证运行。

真实归档R1在依赖checkout的未跟踪 `examples/example-trust-anchor.cert` 被拒绝，未创建bundle。R2改用三库新建detached checkout；保留开发目录原样，不把工作目录生成物带入交付。

工具R1：19 PASS / 5 FAIL；首边界为旧build-record测试复用历史host gate后与current workload不匹配，原始 `.codex-tmp/source-handoff-20260906/tool-checks-r1/output.log`。修复测试隔离后在新目录重跑；新source helper、三库sealer及模板相关用例已通过。模板初轮注释解析失败与wheel输入闭合复审见 `docs/failure-log.md`，不改写历史实验资格。

- [ ] D001 Dependency source checkpoints and compatibility
- [x] D002 Portable source inputs and definition
- [x] D003 Shared skills and receiving-machine instructions
- [ ] D004 Validated local package and GitHub Experimental publication

Spec182保持0/17；Tiger B003与历史Local R8保持未完成/FAIL。本记录完成后补充精确commit、命令和下一台机器的步骤。
