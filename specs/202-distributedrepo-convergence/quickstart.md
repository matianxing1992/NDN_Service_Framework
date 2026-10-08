# Quickstart Validation Guide

本文件是后续实施指南；本轮没有运行下列build/test/network命令。产品修复与当前convergence未PASS前，只允许focusedrepair，不启动fullqualification。

## Readiness

读取featurepointer→spec/plan/tasks→gap-audit/contracts，核对git/sourcehash及CodeGraph。执行Spec Kit结构/共享入口检查；按T001逐入口冻结baseline。按照DB002–004真实native red/repair并read-only复审后，unprivileged以≤j4构建实际registeredtarget，不能并发同一tree。

## Focused Native Selectors

现有targets：`spec192-repo-inapp-api`、`spec192-repo-persistent-api`与nativelegacyimportunit/integration（先对照tests/wscript确认实际target/source）。新增C++case名称在实现中以行为命名，并记录确切`--run_test`selector与timeout；不得仅help/CLIinvalidschema或emptytestselection声称nativePASS。

postrenamefault必须assertactualsyscallhookhit，检查destinationoldinode/bytes或absence、verifiedbytes、residue/recovery及healthyfollowup。workercase使用实际注册B handler的boundedbarrier，drain后再析构Face；importcase确认postpublish失败后的replayparentbarrier实际hit。

## Candidate Freeze and MiniNDN

T009完全部affected/dynamic/freshaudit后才freeze。复用repo现有preflight/runner而不更改SVStiming；`--preflight-only`需要fresh`--output-dir`，只打印hash，不产生lock；firstfullrun产生lock，sameprofile后续run显式引用。

典型CLI形状（仅指南，替换实际freshpath/已审查case）：

```bash
sudo -n -E /usr/bin/env -u NDN_LOG \
  -u NDNSF_SPEC192_NDN_LOG_TRACE_PROVIDER -u NDNSF_SPEC192_NDN_LOG_TRACE_CHILD \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper:$PWD/NDNSF-DistributedInference" \
  LD_LIBRARY_PATH="/usr/local/src/spec192-ndn-svs-9f2d8a4/build:$PWD/build:/usr/local/lib:/opt/gstreamer-1.20/lib" \
  /usr/bin/python3 tests/minindn/run_spec192_real_cpp_repo.py \
  --campaign-case A06_LARGE --output-dir "specs/202-distributedrepo-convergence/evidence/<fresh-run-id>"
```

需要重核pinnedpath，变量必须置于sudo之后；no-outputreuse，runner自行创建目录。每profile/topology用正确锁；从candidate-and-validation合同生成wholematrix两round计划，含O17互斥子场景、cross-runseed/read。核对manifest的finitepeers/expectedserviceSIGTERM、actualloadedlibs、pcap/signatures、完整storage/digest与cleanup，不靠sudoexit或一行PASS。

## Expected Completion

T002–T008行为closing与T009门全部明确通过，T010matrix全59case及子场景当前两round为PASS，旧evidence只引用history。保留首失败/Changedgate与无抑制sanitizer分类，不把未知powerloss/Tiger/性能算作已验证。
