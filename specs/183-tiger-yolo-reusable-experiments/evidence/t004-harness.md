# T004 Frozen Harness Checkpoint

Date: 2026-09-07
Status: PARTIAL / runtime qualification NOT_RUN

## Implementation

新增`runtime/yolo_bundle.py`，提供显式14文件脚本清单的freeze/verify。复用现有
内容校验owner `_file_identity`，先检查全部输入，再复制核对过的字节到全新目录。
不hardlink可变工作树，不复制模型/SIF/原始结果。清单最后写入，fsync并只读化。
清单可以位于源码树外，路径本身不改变绑定的内容；每个输出只创建一次。

已接入真实`check_operator_profile`的dispatch路径：要求profile和E平面的
harnessManifest字节身份一致，再校验冻结树。错误退出2，合成字节正确仍退出78；
没有使用只读bundle冒充源审批、实际运行资格或模型结果。

## Red / Green And Coverage

初始缺freeze模块、dispatch不返回harness校验、外部manifest/source-root接口缺失
均通过实际tracer暴露后实现。审查发现最初递归扫描所有目录会先遍历未登记
子树；真实Python audit事件回归复现，改为只扫描登记目录并在入口拒绝其他树。
最初仅patch os.scandir不足以覆盖Python3.8 pathlib保存的访问器，故改用真实
audit事件，避免一个实际上未观察到文件扫描的测试假绿。

17项bundle测试：工作树变化不影响冻结副本、外置清单、缺/额外/越界字段、
布尔bytes、错hash、链接、binary/PEM/超限、后续额外文件/树/改写/可写模式、
已有目录不覆盖、写盘失败保留且不可验证/复用。
CLI新增4项，验证dispatch确实检查冻结树、E绑定、注入额外oracle文件与可写
脚本。它们不是单独测试一个未被入口使用的validator。

```text
python3 -m pytest -q Experiments/TigerCluster/tests --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-harness-freeze-r2/junit.xml
```

Exit0，**260 passed in 17.17s**（原239 + bundle17 + CLI4）。r1为259 passed
in17.51s，早于外置清单测试，独立保留；JUnit/raw位于ignored results。
所有新包是明确合成文本fixture，不是实际可运行YOLO/SIF。未下载或复制大工件，
未构建、上传、申请Slurm或执行模型。

## Remaining / Next

T004仍unchecked，未生成真实enabled profile或完整生产bundle。T005/T006的应用
和collector尚未实现，冻结清单故意要求这些真实文件，不用占位脚本凑完整性。
应先用现有T004接口推进T005/T006，再回填五命令最终接线/签名输入/真实receipt，
否则要求T004所有命令完全可执行后才实现被它调用的应用/collector会产生循环。
T007仍须一次审查完整生产路径；所有正式环境资格仍NOT_RUN。

Context active guard通过，CodeGraph先定位既有bundle owner，Spec Kit采用当前
任务/合同；GSD沿用已记录的degraded/W019仓库handoff，不恢复旧Spec168。没有新
统计实验或ARS结果结论。
