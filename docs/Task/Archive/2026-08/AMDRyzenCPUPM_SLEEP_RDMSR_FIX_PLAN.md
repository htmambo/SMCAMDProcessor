# AMDRyzenCPUPM SLEEP_RDMSR_FIX 任务计划

> **Status**: ✅ Completed (completion time: 2026-08-31)
> **归档位置**: [`Archive/2026-08/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md`](../Archive/2026-08/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md)
> **实施 kext 版本**: 0.7.5

## 一句话总结

移除 `setPowerState(0)` 路径上 `getCPBState()` / `dumpPstate()` 同步读 MSR
对 `cpu_lock` 的抢占；删除 `restoreWakeState` 末尾 master `dumpPstate`；新增
`initWorkLoop` 首轮 HWCR 监控埋点；版本号 0.7.4 → 0.7.5。

## 子任务落地

- A. 移除 setPowerState(0) 路径 read_msr — ✅（同步删除 -amdpdbg 守卫的 IOLog）
- B. 复核 wake 路径 P-state 收集 — ✅（initWorkLoop 首轮已实现）
- C. 移除 restoreWakeState 末尾 master dumpPstate — ✅
- D. 版本号 0.7.4 → 0.7.5 — ✅
- E. initWorkLoop 首轮 HWCR 监控埋点 — ✅

## 外部 Review 结果

- Plan: APPROVED（Round 3/5，coding-bridge）
- Code: 条件性批准（视为 APPROVED + observations，Round 2/2，coding-bridge）

## 未解决的 Tech Debt

1. `restoreWakeState:805` 在 `mp_rendezvous` 屏障内 read_msr — 0.7.6 处理
2. setPowerState 路径无锁化导致"崩溃前黑盒期" — 0.7.6 引入无锁 Ring Buffer

## 验收窗口

- 已通过：编译、grep 自检（setPowerState(0) 无 read_msr / IOLog）、diff stat
- 待用户：72h 现场运行验证（按 PLAN §10 T6）；通过后通知关闭 issue
