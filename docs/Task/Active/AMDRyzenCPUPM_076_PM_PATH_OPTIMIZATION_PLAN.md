# AMDRyzenCPUPM 076_PM_PATH_OPTIMIZATION 任务计划

**状态**: 🟡 分析完成（已过 1 轮独立核验并修正），待实施
> 目标版本: kext **0.7.6** | 当前基线: **0.7.5**（commit `17a0af8`）
> 对应 fullauto 状态: 待创建（`.omc/fullauto/amdpowermgmt-076-optimization/`）
> 前序任务: [`AMDRyzenCPUPM REVIEW_FIX`](../Archive/2026-10/AMDPOWERMGMT_REVIEW_FIX_PLAN.md)（已完成）
> 事故背景: [`AMDRyzenCPUPM SLEEP_RDMSR 根因复盘`](../Archive/2026-08/AMDRyzenCPUPM_SLEEP_RDMSR_ROOTCAUSE.md)

## 一句话总结

把 **关中断上下文中的 MSR 访问**（idle 回调 + 全核屏障）从 PM 路径清除，
把**唤醒恢复**从 PM 回调线程搬到 work loop 单线程串行执行，
**修掉两处可由用户态触发的内核 panic 路径**（空函数指针 / 未校验符号），
并修掉 `pmRyzen_stop()` 死循环与功率换算错误。

## 任务目标

1. **消除内核 panic 可达路径**（O20/O21）：用户态一次调用即可触发空函数指针调用。
2. 消除 PM 路径上剩余的「关中断 + 同步 MSR 访问」组合 —— 这正是 2026-08-26
   ticket spinlock panic 的形状（仓库根因文档已定性）。
3. 解决仓库已登记的两条 0.7.6 tech debt：`restoreWakeState` 的 MSR 读、
   以及"崩溃前黑盒期"（无锁 Ring Buffer）。
4. 修正确定性缺陷（死循环、功率换算、掩码、缓存分歧、越界、引用漂移）。
5. 不改变用户态 ABI（selector 编号、SleepState 布局、结构体大小）。

---

## 一、现状审计（2026-10-29）

### 1.1 审计范围与方法

| 项 | 内容 |
|---|---|
| 审计对象 | `AMDRyzenCPUPowerManagement/`（cpp / hpp / pmAMDRyzen.c / UserClient） |
| 审计维度 | PM 回调路径、全核屏障路径、idle 回调路径、定时器 tick 路径、用户态写路径 |
| 判定基准 | 仓库自述根因（`SLEEP_RDMSR_ROOTCAUSE.md`）：**PM 路径不得在关中断上下文同步访问 MSR**；**PM 路径不引入新锁**；**PM 路径不 panic** |
| 主审 | 两遍自检（逐条以行区间提取复核，含屏障内容物化核对） |
| 独立核验 | 只读子代理，15 条逐条判定 + 独立复查（Round 1，结论 **PARTIAL**，见 §11） |

### 1.2 核心结论（O 系列）

| 编号 | 位置 | 现象 | 严重度 | 主题 |
|---|---|---|---|---|
| O1 | `pmAMDRyzen.c:249,290-296,116-131` + `cpp:564-576` | idle 回调在**关中断**上下文经 `wrmsr_carefully` 写 PStateCtl（仅在 P-state 实际跃迁时触发） | **BLOCKER** | A |
| O2 | `cpp:231`（屏障 `cpp:201-240`） | 常规 tick 的 `updateInstructionDelta` 在 `mp_rendezvous_no_intrs` **屏障内**读 MSR（每 tick × 每逻辑 CPU；SMT 兄弟核同样执行） | **MAJOR** | A |
| O3 | `cpp:143,145,154,164`（屏障 `cpp:124-170`） | init 块的**屏障部分**（开机 + 每次唤醒）做 HWCR 读+写、P-state dump（8 次读）、APERF/MPERF 读；dumpPstate 仅 master 执行 ⇒ 其余 15 核关中断空转等待 | **MAJOR** | A |
| O4 | `cpp:209-216` | HWCR 重试**无次数上限**：永久读失败时每 tick（50–1200 ms）在屏障内读+写一次，永久持续（对比 P-state dump 重试有界） | **MAJOR** | A |
| O5 | `cpp:218-229` | P-state dump 重试在屏障内做 8 次读（有界 ≤20 次，仅 master） | **MINOR** | A |
| O6 | `pmAMDRyzen.c:140-144` | `pmRyzen_PState_reset()` 的 `mp_rendezvous_no_intrs` 内经 `set_PState` 写 MSR | **MINOR** | A |
| O7 | `cpp:954`（屏障 `cpp:967` 在其后） | `restoreWakeState` 在 PM 回调线程同步 `read_msr(kMSR_HWCR)` —— 代码自带 TODO(0.7.6) | **MAJOR** | B |
| O8 | `cpp:501-535` vs `cpp:288-294` | PM 回调的全核屏障可能与**在途 tick** 的屏障重叠（`mp_rendezvous` 不可重入；`stopWorkLoop()` 只 `disableAllEventSources()`，不等待在途 handler） | **MAJOR** | C |
| O9 | `cpp:193` | 唤醒后首个 tick 无条件 `PStateCtl = 0`：缓存与硬件分歧 —— selector 9 显示 0、selector 4 显示真实 P-state，**同一 App 内两个接口自相矛盾** | **MINOR** | B |
| O10 | `cpp:713,758,887` | 三条用户态写路径各自独立 `mp_rendezvous`；调用线程是**用户态 client 线程 / kext start 线程 / PM 线程**，与 work loop 线程的 `mp_rendezvous_no_intrs` 无互斥 | **MAJOR** | C |
| O11 | `pmAMDRyzen.c:384-390` + `220-224` | `PMRYZEN_IDLE_MWAIT` 未定义 ⇒ `exit_idle` 恒返回 true ⇒ `pmRyzen_stop()` **死循环**（每次迭代发 IPI）。**触发条件受限**：常规 kext 卸载被 `cpp:1048-1051` 的 `KERN_FAILURE` 拒绝，只有 `IOService::stop()` 被调用才进入 | **MAJOR** | D |
| O12 | `cpp:376-377,823,827-832` | RAPL 换算：① `e *= pwrTimeUnit * 1000` 是无量纲依据的额外因子（LHM 参考实现无此因子）；② 读 MSR 失败返回值被丢弃 → `energyValue=0` → 走回绕分支 → **巨大假值 → 功率尖峰**；③ 回绕公式丢掉当前值 | **MINOR** | E |
| O13 | `pmAMDRyzen.c:108,111` vs `cpp:867,894,940,977` | DfsId 掩码 `0x1f` 与全仓 `0x3f` 不一致（`pmAMDRyzen.c` 是唯一异类）；DfsId 为 bits 13:8（6 位）。实害有限（P0 的 DfsId 通常 <0x20），但该函数把派生出的 P1 FID **写回** MSR（见 O26） | **MINOR** | E |
| O14 | `cpp:164-168` + `637-645` | 唤醒后 APERF/MPERF 读失败时**无 else 分支** ⇒ `freqCountersSeeded` 仍为 true、基线仍是睡眠前旧值（陈旧基线未失效）。多数情况被 `cpp:654` 的 `APERF <= lastAPERF` 拦截并重新基线，**不会发布坏值**；仅在睡眠期间计数器继续走（S0ix/DarkWake）才跨越间隙 | **MINOR** | E |
| O15 | `cpp:45-53` | `init()` 查找 `_wrmsr_carefully` 无限重试 ⇒ 符号缺失时 kext 永不发布；`kextloadAlerts` 每轮自增但**告警只在用户态弹** ⇒ 用户永远看不到；每次重试还重跑 `find_mach_header_addr()` | **MINOR** | E |
| O16 | `cpp:585-608` / `cpp:43,64` / `cpp:623-632` | 死代码：`updateClockSpeed` 无调用点；`pmRyzen_symtable_ready` 写而不读（`cpp:399-401` 等待循环已注释）；APERF/MPERF 内联汇编先读 APERF 后读 MPERF（与 LHM 相反） | **NIT** | F |
| O17 | （登记项）`SLEEP_RDMSR_FIX_PLAN.md:28` | `restoreWakeState` 的 MSR 读 —— 仓库已登记为 0.7.6 待办 | **MAJOR** | B |
| O18 | （登记项）`SLEEP_RDMSR_FIX_PLAN.md:29` | PM 路径无锁化导致"崩溃前黑盒期" ⇒ 需引入无锁 Ring Buffer | **MEDIUM** | G |
| O19 | `cpp:723-727,778-789` + `UserClient.cpp:187,302` | App 每秒轮询 selector 4/11 ⇒ 每秒 1~2 次用户态 `rdmsr_carefully`（`getPowerControl`/`getCPBState`），可由缓存替代 | **MINOR** | E |

### 1.3 独立核验新增发现（O20~O30）

| 编号 | 位置 | 现象 | 严重度 | 归属子任务 |
|---|---|---|---|---|
| O20 | `cpp:410-418` + `UserClient.cpp:76,104` | `_KUNCUserNotificationDisplayAlert` 解析失败时 `kunc_alert` 保持 `nullptr`，但 `hasPrivilege()` 与 `externalMethod()` 仍无条件 `(*(fProvider->kunc_alert))(...)` ⇒ **用户态一次调用即可触发内核 panic**（与 `cpp:50` 的 `kextloadAlerts++` 组合时首次调用即 panic） | **HIGH** | T9 |
| O21 | `cpp:422` + `pmAMDRyzen.c:151,152,155,161,217,222` | 一整批**未校验的符号解引用**：`_tscFreq` / `_pmDispatch` / `_pmUnRegister` / `_i386_cpu_IPI` 缺失即空指针解引用（只有 `_wrmsr_carefully` 被检查） | **HIGH** | T9 |
| O22 | `cpp:734-762,953-972` | HWCR 是 **per-CPU MSR**，却被"读一个 CPU → 写全部 CPU"广播：其余核 HWCR 的其他位（含 init 块逐 CPU 设置的 bit30，`cpp:144`）被发起核的值覆盖 | **MEDIUM** | T4 |
| O23 | `cpp:226-227` | **屏障内 IOLog**（`mp_rendezvous_no_intrs` 内的"重试耗尽"日志），直接违反本文件自订规则（`cpp:172-176`）与"PM 路径不 IOLog"铁律 | **MEDIUM** | T3 |
| O24 | `cpp:820-837` | 睡眠/唤醒**未重置能量基线**（`pwrLastTSC`/`lastUpdateEnergyValue` 保留睡眠前值）；S3 后 RAPL 计数器可能从 0 重启 ⇒ 走回绕分支 ⇒ **唤醒后首个功率读数尖峰**；`cpp:830` 的 `seconds` 无零除保护 | **MEDIUM** | T8 |
| O25 | `UserClient.cpp:288-289` vs `cpp:193,718-719` | `PStateCtl` **跨线程写竞争**：用户态先在屏障外写成员再 `applyPowerControl(255)`，而 init 块会无条件清零 ⇒ 用户的 P-state 请求可被静默清零；`applyPowerControl` 还让所有核在屏障内并发写同一成员 | **MEDIUM** | T5 |
| O26 | `pmAMDRyzen.c:106-114` | 开机**静默改写固件 P1 定义**：用裸 `wrmsr64`（绕过 `wrmsr_carefully`）把 `MSR_PSTATE_0+1` 改写为"P0 的 80%"并置 bit63；`cpp:108` 除以 DfsId 无零检查 ⇒ DfsId=0 时浮点除零 ⇒ 垃圾 FID | **MEDIUM** | T8 |
| O27 | `pmAMDRyzen.c:249,290-296,77-81` | idle 回调**带着关中断返回**（唯一一次 `sti` 在 IO_CSTATE 汇编内且紧跟 `cli`，其后整个尾段含 MSR 写全程关中断）；本构建 `.exitIdle = 0` ⇒ pmCPU **没有 idle 退出回调**，与关中断窗口叠加 ⇒ IPI 在该窗口无法被服务；另 `inw $0xf2` 是**读** SMI 触发端口（触发通常是**写**），所谓 C-state 很可能并未真正进入硬件低功耗态 | **MEDIUM** | T2 |
| O28 | `pmAMDRyzen.h:24,103-113` + `pmAMDRyzen.c:186` | `pmRyzen_cpunum_to_lcpu[lcpu->cpu_num]` 无上界检查（>64 核越界写）；`pmRyzen_cpu_phys_num/primary_in_core/is_master` 无 NULL 检查，且被屏障内调用 | **LOW** | T9 |
| O29 | `cpp:946-952` / `ROOTCAUSE.md:206` | **引用漂移**：代码注释指向的 `ROOTCAUSE.md §4 risk #5` 在该文档中不存在；根因文档指向的 `docs/Task/Active/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md` 不存在（实际在 `Archive/2026-08/`） | **LOW** | T9 |
| O30 | `cpp:335` | `cpuCacheL1_perCore = (cpuid_ecx>>24)+(cpuid_ecx>>24)` ⇒ 复制粘贴错误（应为 `cpuid_ecx>>24` + `cpuid_edx>>24`）⇒ 启动日志 L1 容量错误 | **NIT** | T9 |
| O31 | `UserClient.cpp`（全部输出型 selector） | **用户态可控的内核堆溢出**：`structureOutputSize` 进入时是用户态给的缓冲区字节数，代码却把它当成"缓冲区有多大"直接写 ⇒ 用户态给更小的缓冲区即越界写内核堆。（Round 2 发现，既有缺陷） | **MAJOR** | T13 |
| O32 | `cpp:439-441` | **start() 路径上的内核 panic**：`read_msr(kMSR_RAPL_PWR_UNIT)` 失败即 `panic()` ⇒ 违反"PM 路径不 panic"铁律（Round 2 发现，既有缺陷） | **MAJOR** | T9 |
| O33 | `pmAMDRyzen.c`（`pmRyzen_avgload_pcpu`） | 用户态 selector 6 可达的空指针解引用：拓扑 cpu_num 不连续时命中空洞（Round 2 发现，既有缺陷） | **MINOR** | T9 |

### 1.4 主题归并

- **A 关中断上下文中的 MSR I/O** —— O1~O6。0.7.5 的 REVIEW_FIX 只清掉了
  `writePstate` 与 `restoreWakeState` 两处，**屏障内读 MSR 的形态仍在**：
  每 tick 每 CPU 一次（O2）、每次唤醒一组（O3）、无界重试一组（O4）、
  有界重试一组（O5）、P-state reset 一组（O6），以及最危险的
  **idle 回调内写 MSR**（O1）。
- **B 唤醒路径** —— O7/O9/O14/O17。
- **C 全核屏障的并发** —— O8/O10。`mp_rendezvous` 不可重入，屏障有 7 个触发源
  （tick / init / 用户态写 ×3 / PM 唤醒 / PState_reset），彼此没有互斥。
- **D kext 停止路径** —— O11。
- **E 确定性缺陷** —— O12~O15、O19、O24。
- **F 清理** —— O16、O30。
- **G 可观测性** —— O18。
- **H 健壮性（可触发 panic）** —— O20/O21/O28。**优先级最高**：与 PM 时序无关，
  任何用户态调用都可能直接 panic。

---

## 二、问题分析

### 2.1 主题 H：用户态可触发的内核 panic（优先级最高）

**O20**：`start()` 里 `_KUNCUserNotificationDisplayAlert` 解析失败时只打了一条告警
（`cpp:410-418`），`kunc_alert` 保持 `nullptr`（`hpp:263-264`）。而：

- `UserClient.cpp:96-109`：每次 `externalMethod()` 开头，若 `kextloadAlerts != 0`
  就 `(*(fProvider->kunc_alert))(...)` —— 而 `kextloadAlerts` 恰恰在符号缺失时被
  反复自增（`cpp:50`）⇒ **首次用户态调用即空指针调用 → panic**。
- `UserClient.cpp:76`：`hasPrivilege()` 同样无条件解引用 ⇒ selector 15/95/96/97
  触发。

**O21**：符号解析只检查了 `_wrmsr_carefully` 一个，其余全部裸解引用：

| 符号 | 使用点 | 缺失后果 |
|---|---|---|
| `_tscFreq` | `cpp:422` | `start()` 空指针解引用 → 开机 panic |
| `_pmDispatch` | `pmAMDRyzen.c:151,161` | 同上 |
| `_pmUnRegister` | `pmAMDRyzen.c:152,217` | 停止路径空指针调用 |
| `_i386_cpu_IPI` | `pmAMDRyzen.c:155,222` | 同上 |

这两项与 PM 时序无关，是**确定性 panic 路径**，且修复成本极低（加空指针检查 +
降级），应排在全部 PM 时序优化之前。

### 2.2 主题 A：为什么"关中断 + MSR 访问"必须清零

仓库自己的根因文档（`SLEEP_RDMSR_ROOTCAUSE.md` §真正的根因）已经定性：
`rdmsr_carefully` 会在当前 CPU 上取 `cpu_data_t::cpu_lock`（preempt/I/O 同步屏障），
而 PM 框架正在让其他 CPU 拿同一把锁去执行 sleep 序列 —— 于是 ticket spinlock 超时，
`mp_kdp_enter()` 只收到 1 个 NMI ack，整机硬死锁。

0.7.5 之后代码里**仍然存在**这一组合：

```
pmRyzen_machine_idle()                       // cli 关中断
  └─ set_PState()                            // 仅当 P-state 跃迁时
       └─ pmRyzen_wrmsr_safe()
            └─ write_msr()                   // → wrmsr_carefully → cpu_lock   ← O1
```

O1 的触发时机最恶劣：**CPU 正在进入 idle、其余核同时在拿 `cpu_lock` 响应
PM stop/disarm** —— 与 panic 当天的现场结构同构。

同理，`mp_rendezvous_no_intrs` 屏障内所有核都关着中断，此时再取 `cpu_lock`
等于让持锁者无法推进（持锁者需要中断/抢占才能走到释放点）。O3 的形状尤其典型：
`dumpPstate` **只在 master CPU 执行**，于是 8 次 `rdmsr_carefully` 发生在
"其余 15 核关中断空转等待"的状态 —— 与 2026-08-26 事故结构一致。

**结论**：MSR 访问本身不是问题，"关中断上下文里访问"才是。
解法不是加锁（项目铁律：PM 路径无锁），而是**改用不带锁的裸 MSR 访问**。

### 2.3 主题 A 的可行性：仓库内已有先例

`cpp:623-632` 的 `calculateEffectiveFrequency()` 已经用内联 `rdmsr` 直接读
APERF/MPERF（不走 `rdmsr_carefully`），且位于屏障内、每 tick 执行。
`pmAMDRyzen.c:113` 的 `wrmsr64(MSR_PSTATE_0 + 1, ...)` 也是裸写。

即：**裸 MSR 访问在本项目已是既成事实且被验证过**。本方案是把这一做法推广到
其余 MSR 访问点，并补上"MSR 存在性"的安全前提。

### 2.4 主题 B：唤醒路径为何要改结构而非只加缓存

`restoreWakeState()` 现在在 PM 回调线程上做三件事：读 HWCR（O7）、算快照有效性、
进一次全核屏障写回。

- 只做"缓存 HWCR"（O7 的最小修法）能消除屏障外那次读，但**PM 回调线程与工作线程
  的屏障重叠**（O8）依然存在。
- 因此本方案建议**把整个恢复动作下沉到 work loop**：PM 回调只置一个"待恢复"标志，
  唤醒后首个 tick 在**工作线程**内完成恢复。
  这样所有全核屏障（tick / 用户态写 / 恢复）天然串行于同一线程，
  O8 与 O10 一并收敛，且 PM 回调变成纯标志置位（最短路径）。
- 代价：恢复动作延后至多 1 个 tick（当前 50–1200 ms 自适应间隔）。可用唤醒后首个
  tick 强制短间隔缓解。

### 2.5 主题 C：屏障并发模型

当前全核屏障的触发源与线程：

| 触发源 | 线程 | 入口 |
|---|---|---|
| tick 采样 | work loop | `cpp:201` `mp_rendezvous_no_intrs` |
| init 块 | work loop | `cpp:124` `mp_rendezvous_no_intrs` |
| `writePstate` | 用户态 client 线程 | `cpp:887` `mp_rendezvous` |
| `applyPowerControl` | 用户态 client 线程 | `cpp:713` `mp_rendezvous` |
| `setCPBState` | 用户态 client 线程 / kext start 线程 | `cpp:758` `mp_rendezvous` |
| `restoreWakeState` | PM 回调线程 | `cpp:967` `mp_rendezvous` |
| `pmRyzen_PState_reset` | 调用者（start / 用户态） | `pmAMDRyzen.c:143` `mp_rendezvous_no_intrs` |

真正的重叠对：

- (a) **PM 线程 `restoreWakeState` vs 在途 tick 屏障** —— 最危险（O8）；
- (b) 用户态 client 线程 vs work loop 线程；
- (c) 两个用户态 client 线程并发。

### 2.6 主题 E：RAPL 功率换算（含一处已撤销的误判）

**已撤销**：初稿曾判定 `cpp:376-377` 的掩码 `& 0x1f` / `& 0xf` 存在截断，
建议改成 `& 0xff` / `& 0x3f`。**该结论错误，实施前必须撤销** ——
那套位宽是 **Intel** 的 RAPL 布局；**AMD** 的 `MSRC001_0299` 是：

| 字段 | 位域 | 掩码 |
|---|---|---|
| PU (Power Unit) | bits 3:0 | `0xf` |
| ESU (Energy Status Unit) | bits 12:8 | `0x1f` |
| TU (Time Unit) | bits 19:16 | `0xf` |

三源互证：① 本仓 `hpp:104` 自引的 LibreHardwareMonitor `Amd17Cpu.cs` 注释
（`// TU [19:16] / ESU [12:8] / PU [3:0]`，`esu = (eax >> 8) & 0x1F`）；
② AMD E-SMI 文档；③ AMD PPR §14.9。
⇒ **`cpp:376-377` 的掩码是正确的**（`pwrEnergyUnit` 取 ESU ✓、`pwrTimeUnit` 取 TU ✓）。

**真正的问题**（O12）：

1. `cpp:830-831` 的 `e = (pwrEnergyUnit * energyDelta) / seconds` **已经是瓦特**
   （能量 J ÷ 秒）；
2. `cpp:832` 又乘 `pwrTimeUnit * 1000` ⇒ 给功率乘一个时间单位，**无量纲依据**。
   LHM 同段实现为 `energy = energyBaseUnit * pwr; energy /= deltaTime.TotalSeconds;`
   ——**没有这个因子**。
   - 量化影响：系数 = `1000 / 2^TU`；TU=10（常见默认）⇒ 恒定少 2.3%；
     TU=16 ⇒ 少 98.5%。是**系统性缩放误差**，不是随机放大。
   - ⚠️ 需对照 AMD PPR §14.9 确认 `MSRC001_029B`（TSC-based energy counter）
     的换算是否真的需要 TU —— 若需要，则应写成**有依据的** TU 修正，
     而不是 `*1000` 这个魔数。**默认对齐 LHM 实现（去掉该因子）**。
3. `cpp:823` 丢弃 `read_msr` 返回值 ⇒ 失败时 `energyValue = 0` ⇒ 因
   `lastUpdateEnergyValue > 0` 而走**回绕分支** ⇒ `energyDelta = UINT32_MAX - last`
   （巨大）⇒ **功率尖峰**（不是"读数为 0"）。
4. `cpp:827-828` 回绕公式丢掉当前值；正确写法为无符号自然回绕
   `(uint32_t)(cur - last)`。LHM 同处为 `(0xffffffff - _lastPwrValue) + totalEnergy`。
5. O24：`pwrLastTSC`/`lastUpdateEnergyValue` 跨睡眠未重置 ⇒ S3 后计数器从 0 重启
   ⇒ 唤醒后首个读数尖峰；`cpp:830` 的 `seconds` 无零除保护。

### 2.7 主题 E：`pmRyzen_init_PState()` 的掩码与副作用

```c
// pmAMDRyzen.c:108
float p0spd = (p0 & 0xff) / ((p0 >> 8) & 0x1f) * 200.0F;   // 0x1f（DfsId 应为 6 位）
// cpp:867（dumpPstate）、cpp:894/940/977（其余全部路径）
int curCpuDfsId = (int)((eax >> 8) & 0x3f);                 // 0x3f
```

DfsId 是 bits 13:8（6 位），`pmAMDRyzen.c` 是全仓唯一异类。实害有限
（P0 的 DfsId 实际通常 <0x20），**真正值得处理的是它的副作用**（O26）：
该函数把派生出的 P1 FID **写回** `MSR_PSTATE_0+1`（`pmAMDRyzen.c:113`，裸
`wrmsr64` 绕过 `wrmsr_carefully`，并置 bit63）⇒ 开机静默改写固件 P1 定义；
且 `cpp:108` 除以 DfsId 无零检查。

### 2.8 主题 D：`pmRyzen_stop()` 死循环

编译宏为 `PMRYZEN_IDLE_IO_CSTATE`（`project.pbxproj:867,915`），**未定义**
`PMRYZEN_IDLE_MWAIT`。于是 `pmRyzen_exit_idle()` 走 `#else` 分支：

```c
#else
    target->arm_flag = 1;
    return true;          // ← 恒为 true
#endif
```

而 `pmRyzen_stop()`：

```c
while(pmRyzen_exit_idle(pmRyzen_cpunum_to_lcpu[i])){   // ← 永不退出
    (*pmRyzen_cpu_IPI)(i);
}
```

⇒ 死循环 + 无限发 IPI。**触发条件受限**：常规 kext 卸载被 `cpp:1048-1051` 的
`KERN_FAILURE` 拒绝，只有 `IOService::stop()` 被调用才进入 —— 因此是
"潜伏缺陷"而非日常故障。

### 2.9 主题 G：崩溃前黑盒期

0.7.4/0.7.5 把 PM 路径上的 `IOLog` 与 panic 全部移除（正确），代价是
**崩溃前没有任何现场信息**。仓库 tech debt #2 要求引入无锁 Ring Buffer：
PM 线程单生产者写入关键事件快照，崩溃重启后由用户态或 kext 导出读取，
且**不得在 PM 路径引入锁、不得 IOLog**。

---

## 三、子任务列表

| # | 子任务 | 对应问题 | 依赖 | 状态 |
|---|---|---|---|---|
| T0 | 核实 XNU `rdmsr_carefully`/`wrmsr_carefully` 是否取 `cpu_lock`；核实 AMD PPR §14.9 的 RAPL 换算是否需要 TU | 前置事实 | — | 🟡 RAPL 位域已核实（三源互证）；XNU 源码本机不可达，仍待核实 |
| T1 | 新增裸 MSR 访问原语 + 启动期存在性探测 | 前提 | T0 | ⬜ |
| T2 | idle 路径裸访问（O1/O6）+ **中断状态契约**（O27） | O1/O6/O27 | T1 | ⬜ |
| T3 | 屏障内采集裸访问（O2~O5）+ **IOLog 外移**（O23） | O2~O5/O23 | T1 | 🟡 **O23 已完成**（屏障内只置标志，屏障外打印）；O2~O5 待 T1 |
| T4 | HWCR 缓存化（O7/O17）+ **广播语义决策**（O22） | O7/O17/O22 | T0 | ⬜ |
| T5 | 唤醒恢复下沉 work loop（O8）+ `PStateCtl` 一致性与竞态（O9/O25）+ 频率种子失效（O14） | O8/O9/O25/O14 | T4 | ⬜ |
| T6 | 全核屏障串行化（O10） | O10 | T5 | ⬜ |
| T7 | `pmRyzen_stop()` 死循环修复（O11） | O11 | — | ✅ **已完成**（2026-10-29） |
| T8 | RAPL 换算修正（O12/O24）+ DfsId 掩码与 P1 改写策略（O13/O26） | O12/O13/O24/O26 | T0 | 🔄 **进行中**（2026-10-29，Q1=b / Q2=a 已决策） |
| T9 | **符号校验与空指针防护**（O20/O21）+ 有界重试（O15）+ 越界/NULL（O28/O33）+ 清理与引用漂移（O16/O29/O30）+ panic 降级（O32） | O15~O16/O20/O21/O28/O29~O30/O32/O33 | — | ✅ **已完成**（2026-10-29，Round 2 后补 F1/F2/F3/F7/F9） |
| T10 | 无锁 Ring Buffer（O18） | O18 | — | ⬜ |
| T11 | 用户态读路径去 MSR 化（O19） | O19 | T4 | ⬜ |
| T13 | **用户态输出缓冲区长度校验**（O31）—— 全部输出型 selector 加 `checkOutputSize()`；selector 92 补 NUL 结束符 | O31 | — | ✅ **已完成**（2026-10-29，Round 2 后追加） |
| T12 | 版本号 0.7.5 → 0.7.6、编译、循环审核、部署、72h 现场验证 | 全部 | T1~T11,T13 | 🟡 版本号与编译已完成；部署与现场验证待用户执行 |

> **第一批（2026-10-29）范围**：T9 + T7 + O4 + O23。
> 这批全部是"确定性缺陷修复"，不依赖 T0 的核实结论，也不改变 PM 时序语义，
> 因此可以独立发货、独立现场验证。
> 规格与决策记录见 `.omc/fullauto/amdpowermgmt-076-optimization/spec.md`。
>
> **第一批的 Round 3 收尾（2026-10-29）**：M1（扇区索引族越界 / `strlen(NULL)`，
> 6 个文件）、M2（`pmRyzen_cpus[]` 索引钳位）、M3（`pmRyzen_init_PState()`
> 整数除零）、m1/m2（注释与实现一致）已全部修完；m3（坏拓扑分支会留下
> 无 dispatch 的系统）**接受残留**，理由见 §十。三项必修都属"用户态可触发的
> 内核 panic"类，与本批"用户态不可再触发内核 panic"的验收口径直接冲突，
> 不修完不能提交。

**实施顺序**：见 §九。

---

## 四、改动设计

### T0 前置事实核实

1. XNU 的 `rdmsr_carefully` / `wrmsr_carefully` 实现是否取 `cpu_lock`
   （仓库根因文档如此断言，需对照 `osfmk/i386/rtf.c` / `locore.s` 原文确认）。
   - 若确认取锁 ⇒ T1~T3 成立（改用裸访问可彻底规避）；
   - 若确认不取锁 ⇒ 主题 A 降级为"缩短关中断窗口"类优化，
     但 **O1（idle 回调内写 MSR）仍然必须改**（裸写零成本，MSR 存在性由探测兜底）。
   - 无论结论如何，**O2~O6 的改动都是纯收益**（去掉屏障内不必要的
     `rdmsr_carefully` 调用，行为等价）。
2. AMD PPR §14.9：`MSRC001_029B` 的换算是否需要 TU 修正 ⇒ 决定 T8.1 是否
   保留某个 TU 因子（默认对齐 LHM：去掉 `*1000`）。

### T1 裸 MSR 访问原语

在 `AMDRyzenCPUPowerManagement.hpp/cpp` 新增：

```c
// 裸 MSR 访问：不取 cpu_lock，不做存在性检查。
// 仅可用于"在本机型上必然存在"的 MSR（17h/19h 必备寄存器）。
static inline bool msr_read_raw(uint32_t addr, uint64_t *value);
static inline bool msr_write_raw(uint32_t addr, uint64_t value);
```

配套新增**启动期存在性探测表**：在 `start()` 中（中断已开启、无屏障）用
`rdmsr_carefully` 逐个探测将被裸访问的 MSR：

- `kMSR_PSTATE_CTL` (0xC0010062)
- `kMSR_HWCR` (0xC0010015)
- `kMSR_PSTATE_0` (0xC0010064) ~ `+7`
- `kMSR_PERF_IRPC` (0xC00000E9)
- `kMSR_APERF` (0xE8) / `kMSR_MPERF` (0xE7)

探测结果存入 `msrAvailable[]` 标志位；**任一裸访问点在使用前检查对应标志**，
不可用则退回原 `read_msr/write_msr` 路径（保持行为可预期，不引入新的失败模式）。

> 设计约束：不新增锁；探测只在启动期做一次；探测失败走降级而非 panic。

### T2 idle 路径裸访问 + 中断状态契约（O1/O6/O27）

1. `pmAMDRyzen.c` 新增 `pmRyzen_wrmsr_raw()`，内部直接 `wrmsr64`（与
   `pmAMDRyzen.c:113` 现有裸写一致）。
2. `set_PState()` 改调 `pmRyzen_wrmsr_raw()`；`pmRyzen_PState_reset()`（O6）
   随之脱离 `wrmsr_carefully`。
3. 保留 `write_msr()`（carefully）供启动期与用户态路径使用。
4. **中断状态契约（O27）**：明确 idle 回调的"进入时中断关 / 返回前恢复"契约。
   - 现状：入口 `cli` → IO_CSTATE 汇编内 `sti; inw; cli` → 尾段（含 MSR 写）全程关中断
     → 返回给 XNU 时仍是关中断状态。
   - 目标：**所有 MSR 写与状态判定完成后、返回前恢复中断**（`sti`），
     避免 PM 框架的 IPI 在该窗口无法被服务（本构建 `.exitIdle = 0`，
     无 idle 退出回调兜底）。
   - 另需核实 `inw $0xf2` 的语义：读 SMI 触发端口通常不能真正进入 C-state，
     若确认无效则应改用 `outw` 或改走 `PMRYZEN_IDLE_MWAIT` 分支。
     **列为 T0 的第三项核实**（会改变 idle 行为，须谨慎评估）。
5. 保留 `write_msr()`（carefully）供启动期与用户态路径使用。

### T3 屏障内采集裸访问 + IOLog 外移（O2~O5/O23）

| 位置 | 访问 | 改法 |
|---|---|---|
| `cpp:231` `updateInstructionDelta` | `read_msr(kMSR_PERF_IRPC)` | 改 `msr_read_raw`（仍需每 CPU 各自读，故留在屏障内） |
| `cpp:143-148` HWCR 初始化 | `read_msr` + `write_msr(kMSR_HWCR)` | 改裸访问（探测已确认存在） |
| `cpp:164` APERF/MPERF 播种 | `read_msr` ×2 | 改裸访问（与 `cpp:623` 既有裸读一致） |
| `cpp:211-214` HWCR 重试 | `read_msr` + `write_msr` | 改裸访问 **+ 加次数上限**（O4：当前无界） |
| `cpp:154` / `cpp:225` P-state dump | `dumpPstate()`（8 次读） | 移出屏障（见下） |
| `cpp:226-227` 重试耗尽日志 | `IOLog` | **移出屏障**（O23）：与 `cpp:177-190` 的做法一致 —— 屏障内只置标志，屏障外打印 |

**P-state dump 的特殊处理**：`dumpPstate()` 只在 master CPU 上执行，
其 MSR 值全 package 一致（既有假设 D2），因此**不需要在屏障内**。
改法：与 `writePstate()`（`cpp:905-914`）已有的做法一致 —— 在屏障内只置标志，
**在调用线程（work loop）上、屏障外**完成 dump。

### T4 HWCR 缓存化 + 广播语义决策（O7/O17/O22）

新增成员：

```c
uint64_t hwcrCache = 0;
bool     hwcrCacheValid = false;
```

刷新点（三处，均在中断已开启、无屏障的上下文）：

1. `start()`：读一次 HWCR 并缓存；
2. `setCPBState()`：成功写全核后，用**实际写入的值**刷新缓存；
3. init 块：HWCR 初始化写入成功后刷新（bit 30 置位会影响后续 CpbDis 判断，
   必须同步到缓存）。

`restoreWakeState()`：改为**只读缓存**，按 `sleepState.cpb` 改写缓存副本的
CpbDis 位（bit 25），再在全核屏障内写回。缓存无效时跳过 CPB 恢复
（沿用现有"读失败则跳过"的降级语义）。

**广播语义决策（O22）**：HWCR 是 per-CPU MSR。现有 `setCPBState()` 是
"读调用 CPU 的值 → 写全部 CPU"，会覆盖其他核 HWCR 的其他位（含 init 块
逐 CPU 设置的 bit30）。两个选项：

- (a) 保持现状（读一个写全核），但**只广播 CpbDis 位**（bit 25），
  其余位用各 CPU 自己的值 —— 需要在屏障内逐 CPU 读一次 HWCR 再写，
  成本是"屏障内一次裸读"（可接受：裸读无锁）；
- (b) 明确接受"以发起 CPU 的 HWCR 为准"，并写进注释与文档。

推荐 **(a)**：裸读无锁，代价可控，且消除 per-CPU 寄存器被跨核覆盖的隐患。

### T5 唤醒恢复下沉到 work loop（O8/O9/O25/O14）

- 新增 `bool wakeRestorePending = false;`。
- `setPowerState(1)`：**不再直接调 `restoreWakeState()`**，只置
  `wakeRestorePending = true` 并 `resumeWorkLoop()`。
- work loop tick 开头检查该标志：在**工作线程**上执行恢复
  （HWCR 用缓存 → 无 MSR 读；P-state 写回仍需一次全核屏障，但此时
  与 tick 的屏障同线程 ⇒ 串行，无重叠）。
- **`PStateCtl` 一致性（O9）**：区分"首次开机初始化"与"唤醒后重初始化"：
  - 首次开机：保持现状（`PStateCtl = 0`，P0）；
  - 唤醒后：**不再把缓存清零**，而是把缓存中的当前选择重新写回 MSR，
    保证缓存与硬件一致（用户态 selector 9 显示正确）。
- **竞态修复（O25）**：`applyPowerControl()` 目前让所有核在屏障内并发写
  成员 `PStateCtl` / `sleepState.PStateCtl`。改为：
  - 在**屏障外、调用线程**先算好目标值，写入成员；
  - 屏障内只写 MSR（只读成员，不写）。
- **频率种子（O14）**：唤醒后若 APERF/MPERF 读取失败，把
  `freqCountersSeeded[physical]` 置回 false，使下一 tick 重新播种。

> 风险：恢复延后最多 1 个 tick。若现场验证显示 P-state 生效延迟可感知，
> 可改为"PM 回调置标志 + 立即触发一次短间隔 timer"。

### T6 全核屏障串行化（O10）

把三条用户态写路径（`writePstate` / `applyPowerControl` / `setCPBState`）
与 work loop tick 的屏障收敛到**同一个互斥域**：

- **推荐方案（a）**：provider 持有**一个** `IOCommandGate`，三条用户态写路径
  在 gate 内执行全核屏障；work loop tick 也**在同一个 gate 内**执行它的屏障。
  - 保持**同步语义**：用户态调用返回时写入已完成、回读即见新值
    （`ProcessorModel.swift:453-454` 的回读刷新依赖这一点）。
  - gate 由用户线程与 work loop 线程获取，**PM 回调线程永不获取**
    （T5 之后 PM 路径已无屏障）⇒ 不违反"PM 路径无锁"铁律。
- **备选方案（b）**：统一派发到 work loop 线程执行。
  - 结构最简单，但**破坏"返回即可见"语义** ⇒ 需同步改造 App 的回读逻辑，
    本轮不采用。

验收：并发调用 `setPState` 与 `setCPB`（以及 tick 采样）时无 panic、无 assert，
且全核屏障任意时刻至多一个在途。

### T7 `pmRyzen_stop()` 死循环修复（O11）

`pmRyzen_exit_idle()` 在非 MWAIT 分支下不再恒返回 true：按 `cpu_awake`
标志判断（该字段已存在且在 `pmRyzen_machine_idle()` 中维护），
并在 `pmRyzen_stop()` 的循环里加入**最大迭代次数**兜底，
避免任何情况下无限循环。

### T8 RAPL 换算 + DfsId/P1 策略（O12/O13/O24/O26）

1. **RAPL 单位（O12）**：
   - ❌ **不修改** `cpp:376-377` 的位掩码（AMD 布局 ESU 12:8 / TU 19:16，现有代码正确）。
   - 删除 `e *= pwrTimeUnit * 1000;`（无量纲依据），或按 T0 的 PPR 核实结论
     替换为**有依据的** TU 修正（默认对齐 LHM：直接去掉）。
   - `read_msr(kMSR_PKG_ENERGY_STAT, ...)` **检查返回值**，失败则跳过本次采样
     （保持上一次 `uniPackageEnergy`）。
   - 回绕：`uint32_t energyDelta = (uint32_t)(energyValue - lastUpdateEnergyValue);`
     （无符号自然回绕）。
   - ⚠️ 该项会**改变用户态可见的功率数值**。需与 App 侧显示一起验收
     （`ViewController.swift:131` 直接按瓦特输出），并与用户核对新读数合理性。
2. **能量基线（O24）**：睡眠/唤醒时重置 `pwrLastTSC` / `lastUpdateEnergyValue`；
   `seconds` 计算加零除保护。
3. **DfsId 掩码（O13）**：`pmAMDRyzen.c:108,111` 改 `& 0x3f`，与全仓一致。
4. **P1 改写策略（O26）**：`pmRyzen_init_PState()` 在开机时改写固件
   `MSR_PSTATE_0+1`。需要一次显式决策：
   - (a) 保留但加零除保护 + bit63 语义核实（BKDG / LHM 注释
     `PstateEn[63], 1 == enabled`）+ 改用 `write_msr`（可失败可检测）而非裸 `wrmsr64`；
   - (b) 去掉该改写，完全使用固件提供的 P-state 表。
   - 推荐 (a)：保留既有行为但让它可检测、可回退。

### T9 符号校验、防护与清理（O15/O16/O20/O21/O28/O29/O30）

1. **空指针防护（O20，最高优先）**：
   - `hasPrivilege()` 与 `externalMethod()` 中的 `(*(fProvider->kunc_alert))(...)`
     统一改为**先判空**：为 null 时走无弹窗的降级路径
     （`kunc_alert` 不可用 ⇒ `hasPrivilege()` 直接返回 false 或按"仅一次授权"处理；
     `kextloadAlerts` 告警改为 `IOLog`）。
   - 建议在 provider 侧提供统一入口 `void alertUser(const char *fmt, ...)`
     封装判空，避免各处散落。
2. **符号校验（O21）**：`start()` 中对 `_tscFreq` / `_pmDispatch` / `_pmUnRegister` /
   `_i386_cpu_IPI` 全部判空；缺失 ⇒ 走降级路径（禁用依赖该符号的功能）
   或 `start()` 返回 false，**绝不空指针解引用**。
   - `pmRyzen_init()`（`pmAMDRyzen.c:151-161`）同样需要判空。
3. **有界重试（O15）**：`init()` 的符号查找重试加**次数上限**（如 50 次 × 2 s = 100 s），
   超限后降级：`wrmsr_carefully` 留空 → `write_msr()` 走既有 `wrmsr64` 兜底路径
   （`cpp:571-575` 已有此分支），并 `IOLog` 告警，**不无限期挂起启动**。
   - 同时：告警不能再只依赖用户态弹窗（O20 修复后弹窗不可用）⇒ 加 `IOLog`。
4. **越界与 NULL（O28）**：`pmRyzen_cpunum_to_lcpu[lcpu->cpu_num]` 加 `cpu_num < XNU_MAX_CPU`
   检查；`pmRyzen_cpu_phys_num/primary_in_core/is_master` 加 NULL 检查
   （它们在屏障内被调用，NULL 会在屏障里 panic ⇒ 整机死锁）。
5. **清理（O16/O30）**：删除 `updateClockSpeed()`（无调用点）与
   `pmRyzen_symtable_ready`（写而不读）；修正 `cpp:335` 的 L1 复制粘贴错误。
   APERF/MPERF 读取顺序：**LHM 的参考顺序是 MPERF 先、APERF 后**
   （`Amd17Cpu.cs` 的 `ReadPerformanceCounter`）；本项目 `cpp:623-632` 相反。
   仅影响几次指令的偏差，**列为 NIT**：可改为与 LHM 一致，或加注释说明偏差可忽略。
6. **引用漂移（O29）**：修正 `cpp:946-952` 指向的不存在的
   `ROOTCAUSE.md §4 risk #5`；修正根因文档里指向 `docs/Task/Active/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md`
   的失效链接（实际在 `Archive/2026-08/`）。

### T10 无锁 Ring Buffer（O18）

- 单生产者（PM 线程 / work loop）/ 单消费者（用户态或 kext 导出）的无锁环形缓冲，
  容量 256 条，条目为定长快照（时间戳、事件类型、PStateCtl、cpb、
  pstateDumpPending、HWCR 关键位、屏障进入/退出计数）。
- PM 路径**只写内存，不加锁、不 IOLog**。
- 导出方式：UserClient 新增一个只读 selector（追加编号，不改动既有编号），
  或 kext 启动后由用户态工具读取。

### T11 用户态读路径去 MSR 化（O19）

- `getPowerControl()` 改为返回缓存的 `PStateCtl`（kext 侧本就知道当前选择），
  去掉每次轮询的 `read_msr(kMSR_PSTATE_CTL)`；
- `getCPBState()` 改为返回 `hwcrCache` 派生的 CpbDis 位（与 T4 的缓存配套），
  去掉 `read_msr(kMSR_HWCR)`；
- 语义变化：返回的是**期望/已知状态**而非瞬时硬件读数。
  需在 UserClient 注释与 App 侧说明；App 展示不变。

---

## 五、预期效果和验收标准

### 功能与性能

- 目标：**用户态不可再触发内核 panic**（O20/O21 修复后，
  `kunc_alert` / 各符号缺失均走降级路径）。
- 目标：PM 路径上「关中断上下文中的 `rdmsr_carefully`/`wrmsr_carefully`」
  调用次数归零（grep 自检 + `-amdpdbg` 日志计数交叉验证）。
- 目标：唤醒路径（PM 回调线程）不再执行任何 MSR 读。
- 目标：全核屏障在任意时刻至多一个在途（单线程串行）。
- 目标：idle 回调返回前中断状态与 XNU 契约一致（O27）。
- 目标：`pmRyzen_stop()` 可在有限时间内返回。
- 目标：功率读数与真实功耗一致（±10%），无回绕/唤醒尖峰。
- 目标：唤醒后用户态显示的 P-state 与硬件实际选择一致（selector 9 与 4 不再矛盾）。

### 验收项

1. `xcodebuild -scheme AMDRyzenCPUPowerManagement -configuration Release build` 通过。
2. grep 自检：`mp_rendezvous*` 块内不再出现 `read_msr` / `write_msr` / `IOLog`
   （例外：显式注释说明并经审核批准的项）。
3. 循环审核（并发只读子代理，≥2 轮）无 MAJOR/BLOCKER。
4. 用户态 ABI 无变化（selector 编号、SleepState 布局、结构体大小），
   新增能力只能追加新 selector。
5. 现场 72 小时运行验证（参照 0.7.5 验收窗口），覆盖：
   常规睡眠/唤醒、DarkWake 风暴、App 改 P-state、CPB 开关、
   **kext 停止路径**（`IOService::stop()` 可达路径）。

### 5.1 部署与现场验证（第一批）

产物：`build/AMDRyzenCPUPowerManagement-0.7.6.kext.zip`
（二进制 UUID `8045BDAA-FA33-352F-88FB-AB27AA4B3975`，保留符号表，可直接 atos）。

> **boot 关键操作不代执行**（沿用 0.7.4 的记录惯例），请手动安装：

```bash
sudo diskutil mount disk0s1
# 备份旧 kext
sudo cp -R /Volumes/EFI/EFI/OC/Kexts/AMDRyzenCPUPowerManagement.kext \
           ~/AMDRyzenCPUPowerManagement-0.7.5.bak.kext
sudo rm -rf /Volumes/EFI/EFI/OC/Kexts/AMDRyzenCPUPowerManagement.kext
# 解压安装
cd /tmp && rm -rf amdkext && mkdir amdkext && cd amdkext
unzip -q <repo>/build/AMDRyzenCPUPowerManagement-0.7.6.kext.zip
sudo cp -R AMDRyzenCPUPowerManagement.kext /Volumes/EFI/EFI/OC/Kexts/
# 重启后验证版本
kmutil showloaded --list-only | grep -i amdryzen
```

验证要点：

| 项 | 期望 |
|---|---|
| 启动日志 | `AMDCPUSupport::start, Physical Count: 16, Logical Count: 16`（L1 缓存数值此时已修正） |
| App | 能读到 kext 版本 `0.7.6`，无"版本过旧"提示 |
| App 功率读数 | 本批**未改**功率公式（O12 属 T8，未实施），数值应与 0.7.5 一致 |
| App P-state 编辑 | root 运行或带 `-amdpnopchk` 时正常；否则走既有"无权限"提示路径 |
| 睡眠/唤醒 | 无 panic；`pmset -g log` 的唤醒次数与 0.7.5 同量级 |

回滚：把 `~/AMDRyzenCPUPowerManagement-0.7.5.bak.kext` 拷回 EFI 即可。

---

## 六、风险评估和缓解措施

| 风险 | 等级 | 缓解 |
|---|---|---|
| 裸 MSR 访问在不支持的机型上触发 #GP | 高 | T1 启动期逐个探测，探测失败自动退回 carefully 路径 |
| 改动 idle 中断契约影响 PM 行为 | 高 | T0 先核实 `inw $0xf2` 语义；单独小步提交 + 现场验证 |
| 唤醒恢复下沉导致 P-state 生效延迟 | 中 | 唤醒后首个 tick 用短间隔（50 ms）；必要时保留 PM 回调内的最小同步动作 |
| 屏障串行化改变用户态写操作时序 | 中 | 采用方案 a（IOCommandGate）保持同步语义；核对 App 侧时序依赖 |
| 功率换算修正改变用户可见数值 | 中 | 与用户核对；保留 `debugEnabled` 下的新旧双值日志以便对照 |
| `pmRyzen_init_PState()` 掩码修正改变开机 P1 定义 | 中 | 与 O26 的决策一并做；先在 `-amdpdbg` 下打印新旧值对比 |
| 符号降级路径导致功能静默失效 | 中 | 每条降级路径都加 `IOLog` 告警 + 用户态可查询的状态位 |
| 裸访问掩盖真实读写失败 | 低 | 裸路径只用于"必然存在"的 MSR；读写结果靠既有校验逻辑判定 |
| 改动面过大引入回归 | 中 | 按 T9 → T2 → T3 → T4 → T5 分步提交，每步独立编译 + 独立审核 |

**硬约束（继承自 2026-08-26 事故教训）**：

- PM 路径**不引入任何新锁**；
- PM 路径**不做 IOLog**（仅 `-amdpdbg` 且在屏障外）；
- PM 路径**不 panic**（一律降级）。

---

## 七、阶段 0 输出(spec)

- 路径：`.omc/fullauto/amdpowermgmt-076-optimization/spec.md`
- 需包含：`## Assumptions Made` / `## Decisions Made`
- 初版假定：
  - P-state 定义 MSR 全 package 一致（沿用 REVIEW_FIX 的 D2）；
  - `sleepState.cpb` 语义为"期望状态"（沿用 D3）；
  - HWCR 缓存采用"读调用 CPU → 写全核"语义（O22 的 (b) 方案，T4 待决）。

---

## 八、需用户确认的事项

> **2026-10-29 已决策**（Q1~Q4 全部回复，结论见下表"决策"列）。

| # | 事项 | 选项 | 建议 | **决策** |
|---|---|---|---|---|
| Q1 | 功率读数修正后**数值会变化**（当前为系统性缩放，修正后与真实功耗对齐） | (a) 直接修正 (b) 先在 `-amdpdbg` 下双值对照一周再修正 | (b) | **(b)** —— T8 里同时算出"修正值"与"现值"，默认仍对外给**现值**，两者只在 `-amdpdbg` 下并排打印。一周对照确认后，再用单独一次提交把对外值切到修正值 |
| Q2 | O26 开机改写固件 P1 定义 | (a) 保留但加保护 (b) 删除改写 | (a) | **(a)** —— 保留改写，但加保护：改用可失败可检测的 `write_msr`（不再用裸 `wrmsr64`）、值域校验、`-amdpdbg` 下打印改写前后的值 |
| Q3 | O27 idle 路径 | (a) 仅恢复中断状态 (b) 同时改用 MWAIT 分支 | (a)，(b) 另开任务 | **(a)** —— 属 T2，本批不动；本轮只做 T8 |
| Q4 | 唤醒恢复下沉带来的 P-state 生效延迟 | (a) 接受 ≤1 tick (b) 保留 PM 回调内最小同步动作 | (a)，现场验证后再定 | 沿用建议 (a)，**待 72h 现场验证后再定**（不影响 T8） |

---

## 九、实施顺序和依赖关系

```
T9（符号校验 / 空指针防护 / 清理）   ← 最高优先：消除用户态可触发的 panic
T7（stop 死循环）  ┐
T8（RAPL/DfsId）    ├─ 互相独立，可与主线并行
T10（Ring Buffer）  ┘

T0（XNU / AMD PPR 核实）
 └─ T1（裸访问原语 + 启动期探测）
      ├─ T2（idle 路径 + 中断契约）      ← 最高风险，独立小步发货
      ├─ T3（屏障内采集 + IOLog 外移）
      └─ T4（HWCR 缓存 + 广播语义）
           └─ T5（唤醒恢复下沉 + PStateCtl 一致性）
                └─ T6（屏障串行化）

T11（用户态读路径去 MSR 化）← 依赖 T4 的缓存

全部完成 ──► T12（版本号 / 编译 / 循环审核 / 部署 / 72h 验证）
```

**分阶段发货建议**：T9（panic 防护）可作为 **0.7.6 抢先版**单独落地并现场验证；
T2、T7、T8 可各自作为独立小版本落地，不必等 T1~T6 全部完成；
T3~T6 作为 0.7.6 主体一次发布。

---

## 十、外部审核意见

### Round 1（独立只读子代理，2026-10-29）

**VERDICT: PARTIAL** —— 15 条中 13 条成立、1 条误报、1 条前提有误；
另独立发现 13 条遗漏项（O20~O30）。结论集整体可用，**但不能按初稿实施**。

### 已采纳的修正

| 修正项 | 原稿 | 修正后 |
|---|---|---|
| RAPL 位掩码 | 判定 `& 0x1f`/`& 0xf` 截断，建议改 `& 0xff`/`& 0x3f` | **撤销该修改** —— AMD 布局为 ESU 12:8 / TU 19:16 / PU 3:0，现有掩码正确（已由 LHM `Amd17Cpu.cs` + AMD E-SMI + PPR 三源互证） |
| 屏障重叠的线程归属 | "用户态写路径可能与 work loop 线程重叠" | 表述精确化：三条用户态写路径**从不**在 work loop 线程执行；真正危险的一对是 PM 线程 `restoreWakeState` vs 在途 tick 屏障（O8） |
| init 块范围 | "初始化块整体在屏障内" | 改为"屏障部分"（`cpp:172-197` 在屏障外，其中还有一次 `-amdpdbg` 下的 HWCR 读） |
| 能量读失败后果 | "读失败保持 0" | 改为：读失败 ⇒ `energyValue=0` ⇒ 走回绕分支 ⇒ **巨大假值 → 功率尖峰**（严重度上调） |
| `pmRyzen_stop()` 可达性 | "kext 停止/卸载时死循环" | 限定为：常规卸载被 `KERN_FAILURE` 拒绝，触发条件是 `IOService::stop()` 被调用 |
| 频率种子影响 | "首个采样跨越睡眠间隙" | 改为"陈旧基线未被失效"；多数情况被 `cpp:654` 拦截并重新基线，不发布坏值 |
| DfsId 掩码实害 | 未限定 | 限定：P0 的 DfsId 实际 <0x20，截断罕见；真正问题是"派生 P1 FID 并写回 MSR"的副作用（O26） |
| 量纲错误性质 | "依赖错误系数恰好抵消的脆弱巧合" | 改为：固定缩放系数 `1000/2^TU` 的**系统性缩放误差**（TU=10 少 2.3%，TU=16 少 98.5%） |
| O16 文件引用 | `pmAMDRyzen.c:623-632` | 更正为 `cpp:623-632`（`pmAMDRyzen.c` 仅 415 行） |

### 新增问题（已全部纳入 §1.3）

O20/O21（**HIGH**：用户态可触发内核 panic）、O22、O23、O24、O25、O26、O27、
O28、O29、O30。

### 未能独立复核的边界

XNU `rdmsr_carefully`/`wrmsr_carefully` 是否获取 `cpu_lock` —— 本机无 XNU 源码
可达，无法独立复核 ⇒ **O1~O6 的严重度定级**（而非代码事实）不作背书；
该项已自列为 T0 前置核实项，与核验结论一致。

### Round 2（代码阶段，只读子代理，2026-10-29）

**VERDICT: CONDITIONAL** —— 本批声称修好的 9 组问题中 8 组确认到位（O20/O21/O15/O11/O4/O23/T9.5 + 附加项），
无 BLOCKER、无逻辑反转、无 off-by-one、无 ABI/语义变更、无 PM 路径新增 IOLog 或锁；
但**必须合并前修掉 3 项**（均为 O28 缺陷类的残留口子），另有 2 项既有 MAJOR 被顺带发现。

**必须修（已全部修完）**：

| 编号 | 严重度 | 问题 | 修法 |
|---|---|---|---|
| F1 | MAJOR | 新增的 `hwcrInitRetryLeft[cpu_number()]` 无上界检查 ⇒ 越界写发生在**全核屏障内** | init 块与常规 tick 开头加 `cpu_num >= CPUInfo::MaxCpus` 钳位；物理核下标 `physical` 同样钳位 |
| F2 | MAJOR | `cpu_num >= 64` 的跳过让 `pmRyzen_num_phys` 与 `num_logi` 失配 ⇒ `totalNumberOfPhysicalCores` 可 >64 ⇒ 用户态越界读 | 只统计"有 lcpu 注册成功"的物理核；`start()` 追加 `> CPUInfo::MaxCpus` 检查 |
| F3 | MINOR | `pmRyzen_avgload_pcpu()` 裸解引用（用户态 selector 6 可达） | 改走 `pmRyzen_get_lcpu()` + NULL 检查 + 除零保护 |
| F6 | MINOR | 已提交的文档链接指向未纳入 git 的计划文件 | `git add docs/Task/Active/`（提交时必须带上） |

**顺带修掉**：

| 编号 | 严重度 | 问题 | 修法 |
|---|---|---|---|
| F4 | MINOR | `presentAlert()` 用 0 表示"没问到用户"，而 0 是合法按钮下标 | 哨兵改为 `UINT_MAX` |
| F5 | MINOR | `init()` 重试 50×2 s = 100 s 开机卡顿；`kextloadAlerts` 每轮自增导致弹窗语义失真 | 收紧到 20×1 s；`kextloadAlerts` 只在首次失败时自增一次 |
| F7 | MINOR | PM 注册泄漏：拓扑非法导致 `start()` 返回 false 时，pmDispatch 未注销 | 失败分支显式 `pmRyzen_stop()` |
| F9 | NIT | `_tscFreq` 缺失的注释自相矛盾（写"按初始化失败处理"，实际是降级） | 注释改正 |
| P2 | MAJOR（既有） | `read_msr(kMSR_RAPL_PWR_UNIT)` 失败即 `panic()`（start 路径上的内核 panic） | 降级为"功率测量不可用"（`raplValid`），并加入 O32 |
| P1 | MAJOR（既有） | UserClient 全部输出型 selector 不校验用户缓冲区容量 ⇒ **用户态可控内核堆溢出** | 全部改为 `checkOutputSize()`；selector 92 补 NUL 结束符。加入 O31 / T13 |

**未修（记录）**：

- F8（NIT）：`pmRyzen_stop()` 里的 `if(i >= XNU_MAX_CPU) break;` 当前不可达，
  但它保护着 `pmRyzen_cpunum_to_lcpu[i]` 的数组访问 —— 保留作为防御性边界。
- F10（NIT）：`cpu_to_processor` 从未被调用，其判空是死代码 —— 无害，保留告警。
- P4（MINOR，既有）：`pstateDumpRetryLeft` / `pstateDumpPending` 被用户态线程与
  work loop 线程无同步读写 —— 属 T5 范围。

**未能验证**：本机非 Hackintosh 目标机（无 AMD Zen 拓扑、无 XNU PM 调度），
因此 `pmRyzen_stop()` 的实际收敛时序、`cpu_awake` 的跨核可见性、
>64 核拓扑下的越界实际后果、`kunc_alert` 缺失时的端到端行为
**均只有静态推理与编译期证据，无运行时证据** ⇒ §5.1 的 72 小时现场验证仍然必须执行。

### Round 3（代码阶段，只读子代理，2026-10-29）

**VERDICT: CONDITIONAL** —— 本批的修复方向与实现质量整体正确，
F2/F3/F4/F7/F9/P1 已确认修好且与实现自洽；审查期间出现的
`checkOutputSize()` 自递归（必然内核栈溢出 panic）已在审查进行中被修掉、
产物同步。**合并前需修 3 项**，另建议修 2 项。

子代理独立复现了自递归缺陷：把 `checkOutputSize()` 抽出来单独编译运行，
得到 `Segmentation fault: 11 (exit=139)` —— 在 kext 里就是内核栈溢出 panic，
且触发条件是**任意一次**输出型 selector 调用（App 启动即调 selector 8）。

**必须修（M1/M2/M3）**：

| 编号 | 严重度 | 问题 | 触发条件 | 修法 |
|---|---|---|---|---|
| M1 | MAJOR（既有残留） | selector 92 对 `getReadableStringForFan()` 返回值直接 `strlen()`，未判 NULL；且三个 SuperIO 家族的边界检查是 `fan > activeFansOnSystem`（off-by-one），负下标也放行 | 用户态把扇区号传成 > `activeFansOnSystem` ⇒ helper 返回 `nullptr` ⇒ `strlen(NULL)` ⇒ **一次调用即内核 panic**；传 `== activeFansOnSystem` ⇒ `kFAN_READABLE_STRS[fan]` 越界读 ⇒ 读到野指针再 `strlen` ⇒ 同样可能 panic | UserClient 侧先做下标范围校验 + 返回指针判空；三个 SuperIO 家族的 18 处边界检查统一改为 `fan < 0 \|\| fan >= activeFansOnSystem` |
| M2 | MAJOR（F1 未覆盖的同类越界） | `pmRyzen_cpus[XNU_MAX_CPU]` 仍被 `cpu_number()` / `preferredCPU` 无钳位索引 | 逻辑核数 > 64，或 `preferredCPU` ≥ 64 | `pmRyzen_doPState_reset()` 加上界钳位并跳过；`pmRyzen_machine_idle()` 用 `tracked` 判定 + 静态暂存结构兜底未注册核；`pmRyzen_choose_cpu()` 钳位 `preferredCPU` 与 `pmRyzen_last_woken_cpu`；`pmRyzen_get_processor()` 越界返回 NULL |
| M3 | MAJOR（既有残留，与 P2 同类） | `pmRyzen_init_PState()` 整数除零 ⇒ start 路径 #DE ⇒ 内核 panic | `MSR_PSTATE_0` 读失败（`pmRyzen_rdmsr_safe()` 读失败返回 0）或 DfsId 字段为 0 | 分母为 0 时放弃改写固件（降级）；顺带把整数除法改成浮点除法（原式会截断小数，推出来的 P0 频率是错的）；P1 的 FID 加 8 位值域保护 |

**建议修（m1/m2，已修）**：

| 编号 | 严重度 | 问题 | 修法 |
|---|---|---|---|
| m1 | MINOR（文档） | 符号查找重试的注释单位/历史描述三处错误：注释写"每次间隔 1 s ⇒ 上限约 20 s"，实际常量是**毫秒**（总等待 ≈ 20 ms）；注释写"原为 50×2 s"，实际原实现是**无限**重试（每次 2 ms）；最终日志打印 21 与上限常量 20 对不上 | 注释按实际口径改正并说明为什么按 20 ms（内核静态符号不会"晚点出现"，多等只会拖长开机）；日志同时打印实际次数与重试上限 |
| m2 | MINOR（文档） | `presentAlert()` 的 hpp 注释仍写"response 置 0"，与实现的 `UINT_MAX` 矛盾 | 注释改为 `UINT_MAX` 并补上"为什么不是 0"（0 是合法按钮下标） |

**接受残留（m3）**：

`start()` 的坏拓扑分支（`num_phys == 0`）会先注销 XNU 默认 dispatch、再注销自己的
dispatch ⇒ 系统此后**一个 PM dispatch 都没有** ⇒ CPU 永远进不了 idle、无 C-state。
本轮**不改**，理由：

1. 该分支**实际不可达** —— `num_phys` 只统计"有 lcpu 注册成功"的物理核，
   `num_logi == 0` 意味着拓扑全空；`num_phys > CPUInfo::MaxCpus` 在
   "每个被计入的核都贡献 ≥1 个 lcpu、且 lcpu 的 cpu_num < 64"的不变式下不可能成立。
2. 唯一正确的修法需要 XNU `pmKextRegister(version, NULL, &cb)`（"装回默认 dispatch"）
   或二次注册的语义 —— **本机无 XNU 源码可核实**（T0 前置项）。
   二次注册若触发内核 `panic("pmKextRegister: called after PM init")`，
   后果比现状更严重（开机 panic ⇒ 重启循环），违反"PM 路径不 panic"铁律。
3. 待 T0 解锁后再处理：届时把"恢复默认 dispatch"放在
   `pmRyzen_init()` 内、`cb.initComplete()` **之前**，并用静态副本保存默认
   dispatch 函数表以规避"只保存指针"的语义歧义。

**未能验证**：同 Round 2 —— 本机非 Hackintosh 目标机，
M1/M2 的实际越界后果、M3 的除零在真实硬件上是否可触发、
`pmRyzen_stop()` 的收敛时序均**只有静态推理与编译期证据**。

---

## 十一、验收结果

（待填）

---

## 十二、经验教训

1. **位域不能凭"同类寄存器"推断**：初稿把 Intel 的 RAPL 位布局套到 AMD 上，
   差点把**正确代码改错**。教训：跨厂商引用寄存器定义时，必须锁定到
   本仓 `hpp` 自引的那份参考实现（这里是 LHM `Amd17Cpu.cs`），并交叉三源。
2. **"仓库根因文档断言"不等于已验证事实**：`rdmsr_carefully` 取 `cpu_lock`
   这一前提至今没有 XNU 源码原文背书，严重度定级必须显式挂上该前提（T0）。
3. **崩溃路径的优先级高于时序优化**：O20/O21（空指针 / 未校验符号）与 PM 时序
   无关，却是**任何用户态调用都可能直接 panic** 的路径 —— 审计应把
   "用户态可达的 panic" 单独列为一个检查维度，而不是混在 PM 时序里。
4. **只读核验子代理是有效的**：它推翻了 1 条误报、修正了 8 条表述、
   补出 13 条遗漏（含 2 条 HIGH），避免了"按错误方案实施"。

---

## 十三、Runtime Decisions

> 约定：修改下列任一文件前，必须先读本节。

### File: AMDRyzenCPUPowerManagement/AMDRyzenCPUPowerManagement.hpp
- T9（2026-10-29, Phase 1）：删除 `updateClockSpeed()` 声明（无调用点）；
  新增 `presentAlert()`（统一告警入口，判空）；
  新增成员 `hwcrInitRetryLeft[MaxCpus]`（O4 重试预算）、
  `pstateDumpRetryExhausted`（O23 屏障外报告标志）、`tscFreqValid`（O21）、
  `raplValid`（O32 RAPL 单位读取降级）。
- **Round 3 m2（2026-10-29）**：`presentAlert()` 的注释"response 置 0"改为
  `UINT_MAX`，并补上"为什么不是 0"（0 是合法按钮下标，会被误读成用户同意）。
  - Review verdict: Round 2 CONDITIONAL → 已按 F1/F4/F5/F7 意见修完；
    Round 3 CONDITIONAL → m1/m2 已修，m3 接受残留（见 §十）

### File: AMDRyzenCPUPowerManagement/AMDRyzenCPUPowerManagement.cpp
- T9（2026-10-29, Phase 1）：`init()` 符号查找加上限，超限降级到裸 `wrmsr64` 并 IOLog；
  删除 `pmRyzen_symtable_ready`（只写不读）。
- T9（2026-10-29, Phase 1）：`start()` 校验 `_tscFreq`、`_cpu_to_processor`；
  `pmRyzen_init()` 返回 false 或拓扑非法时 `start()` 返回 false（并显式 `pmRyzen_stop()`
  注销 pmDispatch，Round 2 F7）；修正 `cpuCacheL1_perCore` 的复制粘贴错误。
- T9（2026-10-29, Phase 1）：新增 `presentAlert()` 实现 —— 弹窗不可用时降级 IOLog
  并返回 false（"没问到用户" ≠ "用户同意"）。
- O23（2026-10-29, Phase 1）：屏障内 IOLog 改为置 `pstateDumpRetryExhausted`，
  由屏障外打印。
- O4（2026-10-29, Phase 1）：HWCR 初始化重试预算 `PMRYZEN_HWCR_RETRY_BUDGET`(20)，
  每次唤醒重置。
- 引用漂移修正（2026-10-29）：`restoreWakeState` 的 TODO 注释不再指向
  根因文档中不存在的 "§4 risk #5"。
- **Round 2 F1（2026-10-29）**：init 块与常规 tick 开头加
  `cpu_num >= CPUInfo::MaxCpus` 钳位（越界的核直接跳过，不再触碰任何
  定长 per-CPU 数组）；物理核下标 `physical` 同样钳位（>64 物理核机器上
  `pcore_num` 可达 255）。
- **Round 2 F4（2026-10-29）**：`presentAlert()` 的"没问到用户"哨兵从 0 改为
  `UINT_MAX` —— 0 是合法的按钮下标（CFUserNotification 的 default）。
- **Round 2 F5（2026-10-29）**：符号查找重试从 50×2 s 收紧到 20×1 s；
  `kextloadAlerts` 只在首次失败时自增一次（否则弹窗变成 "Kext alert detected: 21"）。
- **Round 2 P2 / O32（2026-10-29）**：start 路径上的 `panic("unable to read power
  unit")` 降级为 `raplValid = false` + IOLog，功率测量停用而不是把机器带走。
- **Round 3 m1（2026-10-29）**：符号查找重试的注释按实际口径改正 ——
  常量单位是**毫秒**（总等待 ≈ 20 ms，不是注释原先写的 20 s）；
  原实现是**无限**重试（每次 2 ms），不是注释原先写的 50×2 s。
  并在超限日志里同时打印实际查找次数与重试上限（退出时 `lookupAttempts` = 上限 + 1，
  只印一个数字会让人对不上代码）。
  - Review verdict: Round 2 CONDITIONAL → 已按 F1/F4/F5/F7/P2 意见修完；
    Round 3 CONDITIONAL → m1/m2 已修，m3 接受残留（见 §十）

### File: AMDRyzenCPUPowerManagement/pmAMDRyzen.c
- T7（2026-10-29, Phase 1）：`pmRyzen_exit_idle()` 非 MWAIT 分支改为按
  `target->cpu_awake` 返回（原先恒 true ⇒ `pmRyzen_stop()` 死循环）；
  `pmRyzen_stop()` 增加 `PMRYZEN_EXIT_IDLE_MAX_SPINS`(1000) 迭代上限、
  NULL 判空、`pmRyzen_cpu_IPI` 判空。
- O21（2026-10-29, Phase 1）：`pmRyzen_init()` 改为返回 `boolean_t`；
  `_pmDispatch`/`_pmUnRegister` 缺失 → 返回 false（服务不发布）；
  `_tscFreq` 缺失 → 优雅降级（`pmRyzen_tsc_freq = 0`）。
- O27-partial（2026-10-29, Phase 1）：`pmRyzen_machine_idle()` 在
  `pmRyzen_tsc_freq == 0` 时跳过负载驱动的 P-state 自动伸缩
  （否则阈值为 0 会把 CPU 钉死在 P0）。中断状态契约部分待 T2。
- O28（2026-10-29, Phase 1）：拓扑遍历加 `cpu_num < XNU_MAX_CPU` 越界检查；
  `pmRyzen_exit_idle()` 加 lcpu/cpu_num 判空。
- **Round 2 F2（2026-10-29）**：`pmRyzen_num_phys` 改为**只统计有 lcpu 注册成功的
  物理核** —— 原来无条件计数，在 >64 逻辑核机器上会出现 num_phys > num_logi
  （甚至 >64），而用户态拿它当 `effFreq_perCore[]` 的循环上界 ⇒ 越界读。
- **Round 2 F3 / O33（2026-10-29）**：`pmRyzen_avgload_pcpu()` 改走
  `pmRyzen_get_lcpu()` 并做 NULL 检查与除零保护（该函数由用户态 selector 6 可达）。
- **Round 2 F9（2026-10-29）**：修正 `_tscFreq` 缺失处理的自相矛盾注释。
- **Round 3 M2（2026-10-29）**：`pmRyzen_cpus[XNU_MAX_CPU]` 的全部索引点加钳位 ——
  `pmRyzen_doPState_reset()` 越界直接返回（该核没注册，本就没有 PState 可重置）；
  `pmRyzen_machine_idle()` 用 `tracked = (cn < XNU_MAX_CPU)` 判定，
  未注册的核改指向静态暂存结构 `pmRyzen_untracked_cpu`（**不能直接 return**：
  idle 回调照样会被 XNU 调用，直接返回会让这些核永远空转），
  且未注册的核不参与负载驱动的 P-state 自动伸缩（暂存结构共用，负载历史无意义）；
  `pmRyzen_choose_cpu()` 钳位 `preferredCPU` 与 `pmRyzen_last_woken_cpu`；
  `pmRyzen_get_processor()` 越界返回 NULL；
  `pmRyzen_last_woken_cpu` 只记录已注册的核。
- **Round 3 M3（2026-10-29）**：`pmRyzen_init_PState()` 的 DfsId 零除保护 ——
  分母为 0 时（`pmRyzen_rdmsr_safe()` 读失败返回 0，或 DfsId 字段本身为 0）
  放弃改写固件而不是整数除零 panic；顺带把整数除法改成浮点除法
  （原式先做整数除法会截断小数，推出来的 P0 频率是错的）；
  P1 的 FID 加 8 位值域保护。**DfsId 掩码 `0x1f → 0x3f`（O13）与
  `write_msr` 化 / 可检测化（O26）留在 T8 一并做。**
  - Review verdict: Round 2 CONDITIONAL → 已按 F2/F3/F8/F9 意见修完（F8 保留为防御性边界）；
    Round 3 CONDITIONAL → M1/M2/M3 已修，m1/m2 已修，m3 接受残留（见 §十）

### File: AMDRyzenCPUPowerManagement/pmAMDRyzen.h
- T9（2026-10-29, Phase 1）：三个拓扑 inline helper 加边界 + NULL 检查，
  新增 `pmRyzen_get_lcpu()`；新增 `PMRYZEN_EXIT_IDLE_MAX_SPINS`；
  `pmRyzen_init()` 返回类型改为 `boolean_t`。
  - Review verdict: 待 Round 2 复审

### File: AMDRyzenCPUPowerManagement/AMDRyzenCPUPMUserClient.cpp
- T9（2026-10-29, Phase 1）：两处 `(*(fProvider->kunc_alert))(...)` 改为
  `presentAlert()`（判空）；`hasPrivilege()` 在弹窗不可用时拒绝；
  `externalMethod()` 的 kextloadAlerts 分支在弹窗不可用时清计数（避免刷屏）。
- O21-连带（2026-10-29, Phase 1）：selector 6 的 `logical/physical` 整数除法
  加物理核数为 0 的保护（除零 ⇒ #DE ⇒ 内核 panic）。
- **T13 / O31（2026-10-29, Round 2 后追加）**：新增 `checkOutputSize()`，
  **全部输出型 selector**（0/1/2/3/4/5/6/7/8/9/11/13/16/17/18/90/91/92/93/94）
  在写结构体输出前先校验用户态缓冲区容量，不够则返回 `kIOReturnBadArgument`
  且一个字节都不写。原先把"想写多少"当成"缓冲区有多大" ⇒ 用户态可控的
  内核堆溢出。
  - 只写不读的 selector（10/12/14/15/19/95/96/97）**不做**校验 ——
    它们通常不带输出缓冲区，校验会误判为错误。
  - selector 92（扇区名）改为写入 `strlen+1` 字节（含 NUL）——
    原先只写 strlen 字节、不带 NUL，调用方按 C 字符串读会越界。
  - 标量输出同样加校验（`checkScalarOutput()`）：selector 2/4/16 会写
    `scalarOutput[0]`，而用户态可以传 NULL 缓冲区指针 ⇒ 内核空指针写。
  - App 侧容量已逐一核对：全部 ≥ 内核需求（selector 0/7=64B、1=32B、
    4=76B、6=64B、16=128B、8=6B …），不改变现有行为。
- **Round 3 M1（2026-10-29）**：selector 92（扇区名）加下标范围校验与
  返回指针判空 —— 扇区下标是用户态给的，必须先确认
  `0 <= fanIndex < getNumberOfFans()`；`getReadableStringForFan()` 对未知下标
  返回 `nullptr`，`strlen(nullptr)` 会 panic。
  - Review verdict: Round 2 后追加；Round 3 CONDITIONAL → M1 已修（见 §十）

### File: AMDRyzenCPUPowerManagement/SuperIO/ISSuperIOIT86XXEFamily.cpp
### File: AMDRyzenCPUPowerManagement/SuperIO/ISSuperIONCT67XXFamily.cpp
### File: AMDRyzenCPUPowerManagement/SuperIO/ISSuperIONCT668X.cpp
- **Round 3 M1（2026-10-29）**：三个 SuperIO 家族各 6 处扇区下标边界检查，
  统一由 `fan > activeFansOnSystem` 改为 `fan < 0 || fan >= activeFansOnSystem` ——
  原判据是 off-by-one（`fan == activeFansOnSystem` 放行 ⇒ 越界读
  `kFAN_READABLE_STRS[fan]`），且负下标（`scalarInput` 是 uint64，转 int 可为负）
  完全不挡。涉及 `getReadableStringForFan` / `getRPMForFan` /
  `getFanAutoControlMode` / `getFanThrottle` / `overrideFanControl` /
  `setDefaultFanControl`。
  - 覆盖的可达路径：selector 92（扇区名，用户态给下标）、
    selector 95（`overrideFanControl`，负下标会越界写硬件寄存器）。
  - 对正常路径零影响：App 侧一律按 `0 ..< getNumberOfFans()` 循环，
    不会取到边界值。
  - Review verdict: Round 3 CONDITIONAL → M1 已修（见 §十）
