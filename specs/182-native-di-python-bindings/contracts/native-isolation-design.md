# Native Runtime Isolation Design

**Status**: DESIGN_FROZEN / O-005 CLOSED; T014 implementation NOT_STARTED
**Owner**: T001 design; T014 harness implementation; T016 qualification
**Authority**: [proof design](proof-design.md#runtime-without-python), [code design](code-design.md)

## Scope

目标是证明规定的DI运行闭包不执行或加载Python，也不把规划/装配/分词转交隐藏服务。测试控制器可以在隔离外使用Python，但不生成业务结果，不进入被测runtime的文件、进程或服务白名单。隔离器不是产品安全协议或通用恶意代码沙箱；不能以工具启动成功替代纯原生业务证明。

采用Linux bubblewrap创建最小只读文件根和独立PID/IPC/UTS/user namespace，外部strace从初始exec跟踪被测进程及全部后代。核对本机bubblewrap0.4.0/strace5.5的实际选项，不能照搬新版未提供的参数。文件根按声明工件逐项构造，不绑定整个`/usr`、`/lib`、home、源码树或venv。解释器、libpython、Python DI包、shell及未声明socket不进入根。

依据：[bubblewrap官方说明](https://github.com/containers/bubblewrap/blob/main/README.md)、[Linux no_new_privs](https://www.kernel.org/doc/html/latest/userspace-api/no_new_privs.html)、[strace手册](https://man7.org/linux/man-pages/man1/strace.1.html)。具体边界由调用参数定义；no_new_privs本身不能限制网络或替代文件隔离。

## Bounded Tool Feasibility Probe

T001只执行工具可行性检查，输出放新`.codex-tmp/spec182-t001-isolation-r1/`。不启动NDNSF、MiniNDN、SIF或Tiger。两例各10秒上限：

1. 正例：外部strace启动bubblewrap `--unshare-all --cap-drop ALL --new-session --die-with-parent`；只把`/bin/cat`作为`/probe/cat`、对应system libc及loader只读绑定，挂载新proc/dev，执行`/probe/cat /proc/self/status`。要求exit0、CapEff=0、NoNewPrivs=1；trace具有成功exec及退出且无未完成截断。
2. 反例：完全相同文件根，尝试执行`/usr/bin/python3 -V`，必须在exec边界ENOENT且非零退出。失败仅表示解释器不可见，不表示完整T014 detector已实现。缺namespace/ptrace权限是工具前置失败，记录UNQUALIFIED并不降级为仅PATH/ldd检查。

probe不复制宿主Python或依赖包，不挂载宿主proc，不启动长驻进程。T014仍需故意fork-helper、libpython动态加载、旁路socket与观测器故障反例。

2026-09-06 R1：正例exit0，`NSpid: 2`、`CapEff: 0000000000000000`、`NoNewPrivs: 1`，trace含`execve("/probe/cat",...) = 0`及`exit_group(0)`；反例exit1，trace为`execve("/usr/bin/python3",...) = -1 ENOENT`。两次supervisor均正常结束，raw有positive/negative.log及各自trace.<pid>。这是两个预先定义的工具检查PASS，反例的非零退出不是产品失败。无NDNSF或网络实验运行。

## Planned File and Function Changes

沿用CD-011/T014既定文件，不新增产品owner或独立测试框架。

| File / operation | Function / parameters / result / responsibility |
| --- | --- |
| ADD `tests/standalone/run-spec182-native-closure.py` | `load_case(manifest_path: Path, case_id: str) -> dict`：读取已冻结case及下面isolation字段，校验ID唯一、schema版本、hash/角色/limits；拒绝未声明路径和未知字段。返回owned validated配置，不填充业务expected |
| same | `stage_root(case: dict, output: Path) -> dict`：output必须为新run-dir；按工件清单复制真实字节和所需ELF loader/DT_NEEDED closure到只读root，校验目标路径无逃逸/循环symlink，返回root路径、file/digest/mode及实际dependency清单。禁止把构建目录整体bind进来 |
| same | `make_launch(case: dict, staged: dict, node: dict) -> list[str]`：生成env-i、外部strace、bwrap和native argv数组；只允许已验证role及transport socket，禁止shell=True和未声明env。node由MiniNDN owner提供实际netns/socket身份，不接受产品返回的“已隔离”声明 |
| same | `run_case(case: dict, staged: dict, output: Path, nodes: dict[str, dict]) -> dict`：nodes为下面的外部node上下文；拥有supervisor、进程组和deadline；spawn前close_fds、新session，持续跟踪trace和startup/业务/cleanup阶段；timeout按冻结预算终止本组，返回原始退出状态/阶段和证据路径，不自行返回PASS |
| same | `collect_trace(case: dict, run: dict) -> dict`：按成功native exec分隔bootstrap与被测scope，解析每个PID的clone/fork/vfork/exec/exit、open/openat/close/dup、mmap/mprotect/munmap和socket/connect；保留unfinished/resumed配对，缺尾/断链/解析未知均UNQUALIFIED。返回events、process identity、violations及完整性 |
| same | `evaluate_case(case: dict, run: dict, observation: dict) -> dict`：同时对照独立oracle、required ordered events、全部角色、cold/warm标记、实际hash、isolation与cleanup；PASS要求所有必需维度均完整。预期业务拒绝必须到达声明的生产边界，preflight/collector错误不能替代 |
| same | `main(argv: list[str]) -> int`：解析现有--manifest/--case/--output契约，依次调用上述函数，写一份result.json；0=完整PASS，1=业务/隔离断言FAIL，2=preflight/observation UNQUALIFIED。异常路径也保存已取得的边界/原始退出码，finally只回收本run资源 |
| ADD `Experiments/NDNSF_DI_NativeClosure_Minindn.py` | `run_campaign(manifest_path: Path, output: Path) -> int`：仅创建封闭拓扑、声明native服务/就绪条件、调用同一closure runner并聚合结果；不复制collector，不提前生成plan/role model/text；不把MiniNDN外部Python进程列入被测native closure |
| ADD `tests/python/test_spec182_native_closure.py` | T014测试上述manifest、trace状态机、判定与清理逻辑；输入为具名positive/negative raw片段，不能用被测evaluate生成expected。真实隔离反例留T016执行 |
| MODIFY planned `tests/fixtures/spec182/case-manifest.json` | 下列isolation对象属于已有case记录；模型/业务输入/expected仍由原case owner冻结，无独立第二份case列表 |

临时`staged`/`run`/`observation`是harness内部owned记录，不能作为产品API或认证证据。staged只读；run由唯一supervisor更新，observation由唯一collector归并；业务进程只产生业务日志和输出，不能写或覆盖outer result.json/trace/manifest。workspace绝对路径只用于此次准备，证据同时保存root内逻辑路径和实际hash。

`nodes`以manifest的node ID为key，每项含`netnsPath: Path`、`netnsInode: int`、`ownerPid: int`、`ownerStartTicks: int`、`nfdSocket: Path`、`peerNodeIds: list[str]`；来自MiniNDN已创建节点的实际/proc身份。run_campaign通过固定仓库路径importlib加载该runner并调用run_case，不以业务callback实现网络/DI工作。run_case打开namespace FD并核对inode/PID starttime后，外部`nsenter --net=<held-fd-path>`进入节点，再启动make_launch返回的观测/隔离argv；FD仅传给可信nsenter且在产品exec前关闭。需要的MiniNDN/netns管理权限属于外部harness，不给产品新增host capabilities。独立CLI使用其专属空网络测试环境的同一结构，不能用宿主netns冒充node。owner死亡、inode变化、socket不匹配均在preflight拒绝。

## Manifest Fields and Limits

| Field / type | Meaning / validation |
| --- | --- |
| `isolation.schema: string` | 固定`spec182-native-isolation-v1`，其他版本拒绝 |
| `isolation.artifacts: list<object>` | 每项`source`、`target`、`sha256`、`kind`（executable/shared-library/data/config）、`mode`；source为受控输入、target是根内规范绝对路径且唯一。ELF根据实际header分类，扩展名不作为可信分类；不把模型数据声明为executable |
| `isolation.processes: list<object>` | 每项`id`、`role`（requester/provider/authority/nfd/repo/controller）、`executable`、`argv`、`env`、`node`；id唯一，executable必须指向已封存artifact。env只允许显式HOME、TMPDIR、LC_ALL和该case已登记的NDN配置；拒绝LD_PRELOAD、LD_AUDIT、PYTHON*、host PATH/venv |
| `isolation.childProcesses: list<object>` | 只登记具名原生子角色：每项`role=assembly-worker`、`executable`（已封存worker artifact）、`parentProcessIds`（具名Provider）、`maxConcurrentPerParent`（case按实际Provider配置冻结）。不允许通配任意exec；collector从clone/exec关系建立实际child PID/starttime身份；没有endpoint权限 |
| `isolation.endpoints: list<object>` | 每项`ownerProcess`、`transport`、`address`、`peerProcessIds`、`purpose`；owner必须在processes中，默认应用只允许独立network namespace中的私有NFD filesystem UNIX socket。无host DBus/X11/SSH-agent/abstract socket、HTTP helper或外部planner |
| `isolation.tools: object` | bwrap/strace/readelf/nsenter的版本及SHA256，实际命令和支持选项；工具位于隔离外。T016固定实际机器值；不把0.4.0/5.5工具探针当所有平台通用资格 |
| `isolation.limits: object` | `runSeconds`取case冻结deadline（默认180）、`cleanupSeconds`默认15、`traceBytes`默认268435456。到期/超量停止并UNQUALIFIED，不截断后判PASS，不运行中加时 |
| `isolation.requiredEvidence: list<string>` | 固定包含identity、process-tree、namespace、exec-map、endpoints、business-oracle、cleanup；不得配置为空或移除项。冷/暖case原有要求额外适用 |

工具执行路径和库清单由安装产品的真实ELF closure计算并与manifest交叉核对，不能使用未安装构建树的ldd输出代替。每次run记录loader、实际映射、NAC/SVS/NDNSD/DI/ORT及模型/config哈希。依赖缺失时preflight拒绝，禁止自动补挂整目录。

## Filesystem, Process and Network Boundary

- bwrap从空根开始，只读绑定已封存文件；新proc仅显示该PID namespace；新dev只提供必要标准设备，CPU资格不绑定GPU设备。独立tmp/work目录可写且不能覆盖代码/配置；home不是宿主home。stdin为/dev/null，继承FD只有stdio和具名bootstrap控制FD，交给产品前控制FD关闭。不绑定宿主`/proc`、`/sys`、源码、缓存或Python资源。
- 默认应用使用`--unshare-all --cap-drop ALL --new-session --die-with-parent`；env由外部`env -i`清空（本机bwrap0.4.0无--clearenv）。记录namespace inode、host PID/starttime、内部PID、CapEff/NoNewPrivs和实际cgroup路径。cgroup不可委托时如实记录，不声称cgroup资源隔离；必需隔离由PID/mount/network namespace提供。
- 每个应用只绑定所属node的独立NFD filesystem socket，新的network namespace隔离host TCP/UDP及abstract UNIX socket。NFD本身在已隔离的MiniNDN node netns中用`--unshare-all --share-net`启动自己的文件/PID沙箱，继承的只是该node netns，不是宿主netns。harness在启动前核对netns inode、接口、无外联default route和NFD peers只属于冻结拓扑；否则UNQUALIFIED。NFD的所有对端也在受观测名单内。
- Repo/Controller/authority必须是封存的原生程序，像requester/provider一样独立跟踪。进程外authority使用已声明NDN服务；内置authority跟随其宿主映射证据。服务名称被允许不等于允许Python实现。harness不得开旁路计算端口、通过NFD提交计算后冒充native owner，源码审查与运行trace共同验证这一边界。
- bubblewrap/strace/MiniNDN均属外部控制工具。strace在产品首次exec之前附着，`-f`跟踪全部后代、保留exec/exit标记（正式runner不使用会隐藏exit标记的`-qq`）。任何未声明成功exec、解释器尝试、非白名单executable mapping、网络连接或root namespace变化均FAIL。即使helper失败后业务fallback成功，也不能PASS。
- 观察器从开始到cleanup不中断。按PID/starttime和clone继承维护FD→实际文件映射，解析execveat/FD执行与dup；mapping的文件hash必须对应封存库。捕获mprotect新增可执行权限；未声明可执行匿名内存/数据文件拒绝。瞬间dlopen/dlclose不可仅靠定时/proc/maps采样，应由完整syscall序列捕获，maps只作为交叉检查。静态嵌入解释器由源码/链接输入清单检查，不能仅凭文件名或动态依赖宣称排除。
- 父supervisor拥有新process group，deadline/异常时先TERM再在cleanup预算内KILL；bwrap的PID namespace回收其后代。等待全部owned PID退出并确认trace结束；不执行全局pkill/mn-c或停止他人服务。私钥与完整原始trace留本地，Git只保存脱敏摘要与hash。

## Required Detector Counterexamples

CD-005的具名[native assembly worker](native-onnx-assembly-design.md)是允许的原生子角色，必须同时满足已封存binary、Provider父身份、固定argv模式和childProcesses策略；它不是Python helper豁免。worker不能连接网络/服务或再exec；I01正例覆盖合法worker，I02/I08覆盖错误binary/parent及超时、晚到child回收。完整产品仍须在T016验证。

| Case | Injection / discriminator | Required outcome |
| --- | --- | --- |
| I01 | 同一封存native小consumer正常完成 | 完整观测后PASS；不能把所有运行都拒绝来伪造有效gate |
| I02 | 旧fork-helper路径；另用重命名Python ELF企图加入输入 | 前者exec尝试被记录且FAIL；后者因非声明来源/依赖身份在preflight拒绝；业务fallback不挽救 |
| I03 | 故意dlopen libpython后立即dlclose；包括重命名.so | 缺失库执行尝试或非白名单ELF映射触发FAIL；缺动态事件采集时不能PASS |
| I04 | 连接host TCP helper、abstract UNIX或未声明filesystem socket | 无host路由/socket且连接尝试被detector拒绝；白名单NFD通路仍成功 |
| I05 | 删除trace尾、遗漏短命child、杀死observer或trace超预算 | UNQUALIFIED / observation boundary；不能被当成预期业务拒绝 |
| I06 | 只提供warm assembled模型、让harness生成plan/text或遗漏一个Provider | 冷准备/生产ordered events/角色覆盖不满足，FAIL；独立oracle不能参与被测计算 |
| I07 | 外部harness使用Python、全部业务进程无Python | 不误报harness；仍要求每个业务owner/白名单服务有完整证据 |
| I08 | 子进程脱离原进程组或启动晚到child | PID namespace/完整后代跟踪保持覆盖；cleanup须无owned存活进程，否则FAIL或观测不足UNQUALIFIED |

T014编写上述unit fixtures及真实反例入口，T015审查实现和边界，T016运行真实反例及全部业务case。O-005仅因工具机制可用且设计/权限/白名单/反例已冻结而关闭；没有宣称T014检测器、PO或最终no-Python资格已通过。

独立selector固定为`tests/python/test_spec182_native_closure.py::test_native_positive`、`test_helper_exec_rejected`、`test_transient_python_mapping_rejected`、`test_undeclared_endpoint_rejected`、`test_incomplete_observation_unqualified`、`test_cold_path_and_role_coverage_required`、`test_external_harness_excluded`、`test_descendant_cleanup_required`，分别对照I01--I08；负例还验证准确boundary/原因，不仅检查exit非零。namespace真实运行由T016拥有，不在T014局部collector unit里偷换成模拟PASS。
