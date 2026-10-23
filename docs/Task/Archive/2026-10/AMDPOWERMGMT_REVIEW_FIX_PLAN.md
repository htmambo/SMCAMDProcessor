**状态**: ✅ 已完成 (完成时间: 2026-10-23)
> 对应 fullauto 状态:.omc/fullauto/amdpowermgmt-review-fix/(state.json 已按规范清理;spec/validation/open-questions 保留审计)

# AMDRyzenCPUPM REVIEW_FIX — 审核发现问题修复

## 任务目标

修复 `AMDRyzenCPUPowerManagement/` 未提交更改(3 文件,+194/-40,0.7.5 睡眠/唤醒路径加固)在第一轮 4 路并发子代理审核中发现的全部问题,并进行循环复审直至无 MAJOR/BLOCKER。

## 问题分析

第一轮审核发现 2 个 MAJOR、10 个 MINOR/NIT(详见 `.omc/fullauto/amdpowermgmt-review-fix/spec.md`)。核心两类:

1. **缓存信任链断裂**:`PStateDef_perCore` 等数组未初始化,新睡眠路径信任缓存而非直接读 MSR,垃圾数据可经弱校验写回硬件 MSR。
2. **同型隐患残留**:`writePstate` 在 rendezvous 屏障内读 MSR(cpu_lock + 关中断 IOLog),与本 diff 在 `restoreWakeState` 消除的死锁形态相同。

## 子任务列表

1. ✅ hpp 三个数组零初始化(PStateDef_perCore / PStateDefClock_perCore / hwcrInitPending)
2. ✅ writePstate 表刷新移出 rendezvous;dumpPstate 返回 bool;pstateDumpPending 重试
3. ✅ HWCR 初始化/重试块检查 write_msr 结果
4. ✅ setCPBState 确认写成功后再缓存;start() 检查返回值
5. ✅ restoreWakeState 全零快照时跳过 PStateCtl 写
6. ✅ freqCountersSeeded 消除虚假首个采样
7. ✅ xcodebuild 编译验证(修复后 ×2、round 2 意见修复后 ×1,均 BUILD SUCCEEDED)
8. ✅ 循环审核 round 2(3 路并发子代理:初始化与重试 / CPB 与 writePstate / restoreWakeState 与整体 —— 全部 APPROVED,2 MINOR 意见)
9. ✅ round 2 意见修复(有界重试 pstateDumpRetryLeft + start() 语义注释)并复审 round 3(增量验证 APPROVED,2 NIT 已修)
10. ✅ 复编译 BUILD SUCCEEDED

## 每个子任务的改动内容

见 spec.md 的修复设计表;实现过程中逐条更新本文件勾选状态。

## 预期效果和验收标准

- xcodebuild 编译 AMDRyzenCPUPowerManagement 通过
- 循环审核 round ≥2 无 MAJOR/BLOCKER
- 无新用户态 ABI 变化(selector 编号 / SleepState 布局 / 结构体大小)

## 风险评估和缓解措施

- 修改均在失败路径/缓存一致性上,不改变成功路径行为
- 不在 PM 路径引入任何新锁(2026-08-26 事故教训)
- 每次文件修改后由独立子代理复审(单文件粒度),同一文件连续 REJECTED 计数,超限写 qa-blocker.md

## 实施顺序和依赖关系

子任务 1→6 互相独立,可并行;7 依赖 1-6;8 依赖 7;9 依赖 8。

## 阶段 0 输出(spec)

- 路径:.omc/fullauto/amdpowermgmt-review-fix/spec.md
- 包含:## Assumptions Made / ## Decisions Made

## 外部审核意见

### 机制说明
coding-bridge MCP 本会话不可用:日志查明启动失败原因为 `spawn uvx ENOENT`(uvx 位于 `~/.local/bin` 但不在桌面 app 的 PATH)。按 AGENTS.md 运行时可用性门禁,该步骤判为"不适用而非失败",三轮审核均以并发子代理(只读 explore,按 diff 切片/修复域划分,控制单次任务大小)执行,未伪造 runReview 调用。

**事后修复**:已将领 `~/.kimi-code/mcp.json` 中 coding-bridge 的 command 改为绝对路径 `/Users/hoping/.local/bin/uvx`(备份 `mcp.json.bak.20261023-uvxpath`),并以同命令预热 uvx 缓存成功(退出码 0)。`/reload` 或 `/new` 后 `mcp__coding-bridge__*` 工具可用。

**终审实跑(2026-10-23 下午)**:经 stdio 直连同一服务端真实调用 7 次,未获可用结论 —— 千帆 glm-5.3 端点对 ≥5KB 请求持续 300s 挂死(重试 3 次 ×4 轮);sensenova glm-5.2 可用但为 thinking 模型,16k/49k 输出预算均被思考链耗尽,随后触发 tpm 配额 429。按失败兜底第 3 档记录:外部审核 MCP 终审未参与,主流程不挂,终审依据为子代理 Round 1/2/3(全部 APPROVED,无 MAJOR/BLOCKER)+ xcodebuild ×3 绿灯。完整证据与后续重跑入口见 `.omc/fullauto/amdpowermgmt-review-fix/mcp-final/VERDICT.md`。

### 轮次结论
- Round 1(4 路):0 BLOCKER / 2 MAJOR / 10 MINOR-NIT → 全部修复(修复设计见 spec.md)
- Round 2(3 路):APPROVED ×3(hpp / cpp / UserClient);意见 = 无界重试暴露窗口 + start() 缓存语义 → 已修复(有界预算 20 tick + 注释)
- Round 3(1 路,增量):APPROVED;2 NIT(耗尽日志、显式 allowLog) → 已修复

## 验收结果

- xcodebuild Release ×3 全部 BUILD SUCCEEDED(仅既有 warning)
- Round 2/3 均 APPROVED,无 MAJOR/BLOCKER 残留
- 用户态 ABI 无变化(selector/SleepState/结构体布局);selector 12 新增 kIOReturnError 上报经 ProcessorModel.swift 验证可优雅处理
- 遗留风险台账见 `.omc/fullauto/amdpowermgmt-review-fix/validation.md`

## 经验教训

- 审核循环中"修全部问题"应区分:代码缺陷必修;语义误解用注释澄清(D3);既有良性风险登记台账不扩大改动面(D4)。
- MCP 工具缺席时先查会话日志定位启动失败原因,常能以最小配置修复(本次:绝对路径),而非整轮降级。

## Runtime Decisions

### File: AMDRyzenCPUPowerManagement/AMDRyzenCPUPowerManagement.hpp
- fix-1: 三个成员数组 `{}` 零初始化 + 新增 pstateDumpPending / freqCountersSeeded 成员 + dumpPstate 签名 void→bool (Phase 2, 2026-10-23, provider=子代理替代,coding-bridge 未配置)
  - Review verdict: 待 round 2 复审
  - 下次执行: must Read this section before modifying hpp

### File: AMDRyzenCPUPowerManagement/AMDRyzenCPUPowerManagement.cpp
- fix-2: initWorkLoop 首轮 HWCR 写结果检查、master dump 赋 pstateDumpPending、APERF 种子置 freqCountersSeeded (Phase 2, 2026-10-23)
- fix-3: 重试 tick 写结果检查 + master 重跑 dumpPstate (Phase 2, 2026-10-23)
- fix-4: start() 检查 setCPBState 返回值 (Phase 2, 2026-10-23)
- fix-5: setCPBState Args 结构收集 rendezvous 写结果,失败返回 false 且不动缓存 (Phase 2, 2026-10-23)
- fix-6: writePstate 表刷新移出屏障至调用线程 (Phase 2, 2026-10-23)
- fix-7: restoreWakeState 屏障前预算 anyPstate,全零跳过 PStateCtl 写 (Phase 2, 2026-10-23)
- fix-8: calculateEffectiveFrequency 首次成功读只播种不采样 (Phase 2, 2026-10-23)
  - Review verdict: 待 round 2 复审
  - 下次执行: must Read this section before modifying cpp
