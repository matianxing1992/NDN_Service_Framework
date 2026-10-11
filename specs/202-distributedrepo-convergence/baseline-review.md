# T001 Baseline Closing Record

**Status**: VERIFIED_STATIC / T001 DONE；产品BLOCK。用户授权完成Spec202，当前Repo HEAD2fcfb415+已有dirtyoverlay。
**Batch growth decision**: B1只关闭逐入口/owner/定义TU/target/sourcebaseline和三个repair前置；不吸收产品代码或网络运行。
**Authority**: project/active Context Mode health均通过（真实新prompt已出现），MCP对应relevance source仍空；仓库fallback。CodeGraph sync后up-to-date。Spec Kit sync11/11及entrypoints11/11。
**Source identity**: `evidence/t001-baseline-20261008/source-before-repairs-complete-with-fixtures.zip` 150inputs，SHA `b756347c14f888b393410f5d7743259f2a19d4f942c1fefb47db830330401dff`；逐文件hash在source-manifest-complete-with-fixtures.json。首次116-file zip明确PARTIAL，不作完整baseline；旧source不回滚。
**Readonly reviewer**: /root/a06_large_review；canonical profile及SHA沿review.md。先header/CodeGraph后Core dirtydelta与native module符号，固定pre-repair源码，无源码写入或build/test。

| Lane | Actual query / files | Closing finding |
| --- | --- | --- |
| callers | Repo公开headers/CodeGraph API→Client→Node，Pythonbindings与DIresolver | api-inventory逐overload，OriginalData fetch/serving explicit O17 |
| implementation/wire | 两fetch/Node802/Importer1820,1837；Core admission/signature delta | 两HIGH+MEDIUM确认，路径独占/leafsymlink、worker drain和双replay前置明确 |
| tests/oracle | tests/wscript、old inventory、59matrix | local/request/object/Persistent分层，exactclosingcases；T002–04 realnative faults待实现 |
| build/definitions | Repowscript/staticarchive、setup.py TU清单、nm-D extension LegacyImport filter | extension单独编译必须重建；无Importer未定义依赖；现有artifact不计新candidate PASS |
| migration/evidence | old192/193 snapshots/status与current150-sourcebaseline | 保留全部intent、overlay/evidence；停止旧执行，当前repair由202维护 |

**Static release**: 审查指出server输出路径与宽泛case占位，均已修正；T002–04 focused release PASS。完整产品/动态/资格不能由baseline静态审查推出。
**Compile/link/runtime/dynamic**: NOT_RUN；final equality与affected consumer重建归T009。已核对actualoutputpath，不虚构rebuilt hash。
**Changed gate**: 逐header overload accounting、明确Python独立TU、两种目的namespace与两replay分支，避免仅看旧inventory/静态archive。
**Supplement / changed gate**: Red review发现旧candidate列表没有clean tests/main.cpp；其原字节由T001 commit685e8efb固定，并保存在同目录clean-test-driver-supplement.zip。baseline完整组成是150-input ZIP加此clean-driver supplement，不能声称150-file ZIP独自包含所有build inputs。T009闭合工具必须对照实际Waf target source/TU列表，包括clean driver与外部fixtures，作为重复source-closure漏检的替代强制检查，不能仅合并dirty/历史candidate列表。
**Retrospective**: static修正遗漏入口和路径；compile/link尚未观测；runtime/test尚未执行；unobserved为新candidate所有行为/动态/资格。
**Closing**: CLOSED_FOR_VALIDATION at B1 staticbaseline；B2/T002开始focused red/repair，产品BLOCK直至全部控制缺口关闭。DeepSeek helper tools/ai/deepseek_delegate.py在当前checkout缺失，采用primary实现+canonical只读reviewer，不调用未知外部路径。
