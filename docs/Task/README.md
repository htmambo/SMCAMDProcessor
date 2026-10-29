# Task Index

## Active Tasks

- 🟡 [AMDRyzenCPUPM 076_PM_PATH_OPTIMIZATION — kext 0.7.6 PM 路径优化方案](Active/AMDRyzenCPUPM_076_PM_PATH_OPTIMIZATION_PLAN.md) — 实施中（2026-10-29）
  - 主线：清除关中断上下文中的 MSR 访问（idle 回调 + 全核屏障）、唤醒恢复下沉 work loop、修复两处用户态可触发的内核 panic 路径、`pmRyzen_stop()` 死循环与功率换算错误
  - 审计出 33 项问题（2 HIGH / 1 BLOCKER / 10 MAJOR / 7 MEDIUM / 9 MINOR / 2 LOW / 2 NIT）；Round 1 核验 PARTIAL（修正 1 项误报 + 8 项表述，新增 11 项）、Round 2 核验 CONDITIONAL（F1/F2/F3 已修，另发现 2 项既有 MAJOR）、Round 3 核验 CONDITIONAL（M1/M2/M3 已修，另发现扇区索引族越界）
  - **第一批已完成并提交**（`91aa480`）：T9（符号校验 / 空指针防护 / 清理 / panic 降级）、T7（死循环）、T13（用户态输出缓冲区校验）、O4（HWCR 重试有界）、O23（屏障内 IOLog 外移）+ Round 3 的 M1/M2/M3 → 版本号 0.7.6，Release 编译通过
  - **第二批（T8）已完成**：O12（RAPL 换算，按 Q1=b 对外值不变、`-amdpdbg` 下双值对照）、O24（睡眠/唤醒重建能量基线）、O13（DfsId 掩码）、O26（开机改写 P1 定义加保护，按 Q2=a 保留改写）
  - **合并上游已完成**（`4c442e7`）：版本号单一来源化（只改 `AMDRYZEN_KEXT_VERSION` 一处）
  - **构建脚本修复已完成**（`fda899b`）：`Scripts/build.sh` 不再因本地 `../sdk` 软链缺失而强制联网下载 Lilu / 克隆 VirtualSMC；`./Scripts/build.sh --clean` 本机 exit 0，产出 `build/Release.zip`
  - **待办**：T1~T6 主线（依赖 T0：XNU 源码本机不可达）、T10（无锁 Ring Buffer）、T11；部署与 72h 现场验证由用户在 Hackintosh 上执行（步骤见方案文档 §5.1 / §5.2）

## Completed Tasks (Archive)

### 2026-10
- ✅ [CI_VERSION_SYNC — CI 改用打包脚本 + 版本号单一来源](Archive/2026-10/CI_VERSION_SYNC_PLAN.md) — Completed 2026-10-28
- ✅ [BUILD_SCRIPT — 本地/CI 一键打包脚本](Archive/2026-10/BUILD_SCRIPT_PLAN.md) — Completed 2026-10-28
- ✅ [AMDRyzenCPUPM REVIEW_FIX — 审核发现问题修复](Archive/2026-10/AMDPOWERMGMT_REVIEW_FIX_PLAN.md) — Completed 2026-10-23
- ✅ [AMD Power Gadget 中文语言包（本地化）](Archive/2026-10/APG_CHINESE_LOCALIZATION_PLAN.md) — Completed 2026-10-23

### 2026-08
- ✅ [AMDRyzenCPUPM 0.7.2 Spinlock Panic — 0.7.4 修复记录](Archive/2026-08/AMDRyzenCPUPM_072_SPINLOCK_FIX_PLAN.md) — Completed 2026-08-26
- ✅ [AMDRyzenCPUPM SLEEP_RDMSR 根因复盘](Archive/2026-08/AMDRyzenCPUPM_SLEEP_RDMSR_ROOTCAUSE.md) — Completed 2026-08-31
- ✅ [AMDRyzenCPUPM SLEEP_RDMSR_FIX 修复实施](Archive/2026-08/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md) — Completed 2026-08-31
- ✅ [AMDRyzenCPUPM NVMe Panic 同步记录](Archive/2026-08/AMDRyzenCPUPM_NVME_PANIC_RECORD.md) — Completed 2026-08-31
