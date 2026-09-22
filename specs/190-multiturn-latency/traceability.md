# Traceability

| Intent / Requirement | Design | Task / planned C++ proof | Success |
| --- | --- | --- | --- |
| 分解异常耗时 / FR-001 | CD-01, TurnTiming | T001 PhaseTiming; T011 matched observations | SC-003,006 |
| 1秒ACK，不盲目加窗 / FR-002, FR-003 | CD-01 | T002 AckWindow; T011 | SC-001 |
| 实时token / FR-004 | CD-02 | T003 LiveTurns; T011 | SC-002 |
| 同handle多轮 / FR-005 | CD-02 | T003 LiveTurns; T011 KV/oracle | SC-003,004 |
| 不等满FINALIZE/收束窗 / FR-006 | CD-03 | T004 TerminalDrain; T011 | SC-003,005 |
| 缓存身份 / FR-007 | CD-04, identity | T005 ResidentSession | SC-004 |
| 短期驻留/退出清理 / FR-008 | CD-04, lease | T005 owner/dynamic tests; T011 | SC-005 |
| 每轮真实证据 / FR-009 | CD-04, evidence separation | T005/T010 stale-profile counterexample | SC-004,006 |
| 不以改workload冒充提速 / FR-010 | CD-05 | T010/T011 C++ oracle + paired raw | SC-003,006 |
| 原生行为证明 / FR-011 | all CD, batch gates | T001–T010 native targets/convergence | all SC |
| 候选身份与范围 / FR-012 | CD-05, invalidation | T010 mutation; T011 immutable run | SC-006 |
| 热路径字节预算 / FR-013 | CD-06, TransferObservation | T006 StageTransfer; T011 | SC-007 |
| layer/assembled/resident分层命中 / FR-014 | CD-06, CD-09 | T005/T006/T009 | SC-007,009 |
| 固定Repo与重启 / FR-015 | CD-07 | T007 PersistentRepoOwner; T011 | SC-008 |
| prepare命中免拆层/导出/STORE / FR-016 | CD-08 | T008 QueryAndReuse; T011 | SC-008,009 |
| 当前授权下复用 / FR-017 | CD-09 | T009 ProtectedMaterialReuse | SC-007,008,009 |

Reverse scope check：T001测量、T002准入、T003交互、T004收尾、T005加载、T010证明有效、T011实测；
T006传输、T007固定存储、T008 prepare复用、T009真实保护路径合法复用，均直接对应用户新增要求。
不扩展GPU、SIF、通用Repo重写或泛化平台。T009安全接口冻结前BLOCK，不能据此宣称所有任务可立即编码。
