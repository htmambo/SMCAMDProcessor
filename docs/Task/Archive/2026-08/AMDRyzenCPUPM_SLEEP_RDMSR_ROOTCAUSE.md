# 2026-08-26 03:42 内核 Panic 事件档案

> 该问题随机发生、可能很久才会复现。若机器再次出现无故重启/panic，请先阅读本文档，
> 将新 panic 与本次结果联合对比分析。

## 结论摘要

2026-08-26 03:42:12（CST），MOREFINE-S500 Hackintosh（Macmini8,1，AMD Ryzen 16 核，
macOS 26.6.2 / Darwin 25.6.0，OpenCore 引导）发生内核 panic：

```
panic(cpu 0): Ticket spinlock[0xffffff80183f6c00] timeout after 383518641 ticks @lock_ticket.c:371
Panicked task: pid 0 kernel_task
mp_kdp_enter() 向全部 16 核发 NMI，仅收到 1 个 ack —— 全机内核态硬死锁
```

## 符号化结果（关键）

panic 线程调用栈落在 **AMDRyzenCPUPowerManagement.kext (0.7.2)**，
UUID `6B3B246C-C495-3555-8074-041365033CFC`（与 EFI 中 `/Volumes/EFI/EFI/OC/Kexts/`
下二进制逐一核对一致，用该二进制 + kext 基址 `0xffffff801b1cb000` 做 atos 符号化）：

| panic 地址 | 符号 |
|---|---|
| `0xffffff801b1d121f` | `AMDRyzenCPUPowerManagement::setPowerState(unsigned long, IOService*) + 399` |
| `0xffffff801b1d1732` | `AMDRyzenCPUPowerManagement::dumpPstate(unsigned long long*) + 1090` |

即 panic 线程正在执行**睡眠前的电源状态回调**：
`setPowerState(0)`（"preparing for sleep"）→ `dumpPstate(sleepState.pstate)`
（`AMDRyzenCPUPowerManagement.cpp:443-462, 725-747`）。

`dumpPstate` 循环对 8 个 P-state MSR 执行 `rdmsr_carefully` 并每次 `IOLog`。
rdmsr 失败时代码里甚至有显式 `panic("...fucked up")`（:730），但本次不是走到那里，
而是在该路径上发生了内核自旋锁死锁（IOLog/console 锁或 CPU 睡眠下线时序均可能参与）。

## 触发场景与伴随现象

- 机器当时 uptime 约 37 小时（自 8-24 14:02 启动）。
- 事发前系统处于**睡眠/暗唤醒风暴**：USB 控制器 `GP17 XHC0 XHC1` 每 ~45 秒把机器
  DarkWake 一次（重启后 6 小时内又发生 362 次）。`setPowerState`/`dumpPstate` 路径
  被极高频触发，显著放大了竞态命中概率。
- 03:35 屏幕曾被唤醒（loginwindow 编译 Metal shader），03:42 panic。
- panic 前无用户操作、无软件更新、无 pmset 定时事件；`last` 无 shutdown 记录。
- `Previous shutdown cause: 5`（"正常关机"）是 VirtualSMC 模拟值，**不可信**，
  以 panic 文件为准。

## 历史/同期不稳定信号（联合分析用）

- 8-21 ~ 8-24：多次 WindowServer `userspace_watchdog_timeout` spin、3 次 `shutdown_stall`。
- 8-25 下午：9 次 GPU 重启（`Kernel_*.gpuRestart`，NootedRed 驱动 iGPU）。
- 8-26 02:36、05:09：`bluetoothd` ExcUserFault 崩溃。
- 8-26 09:01（重启后）：又 1 次 gpuRestart。
- 再上一次内核 panic：2026-08-20 09:07（`Kernel-2026-08-20-090714.panic`，当时未分析，
  如复现可一并符号化对比）。

## 原始证据

- panic 原始文件已备份至本目录：`Kernel-2026-08-26-034212.panic`
- 符号化命令（kext 在 EFI 分区，需先 `diskutil mount`）：

```bash
K=/Volumes/EFI/EFI/OC/Kexts/AMDRyzenCPUPowerManagement.kext/Contents/MacOS/AMDRyzenCPUPowerManagement
dwarfdump --uuid "$K"   # 核对与 panic 头中的 UUID 一致
atos -arch x86_64 -o "$K" -l <kext基址> <栈内地址...>
```

## 排查/修复方向（待办）

- [x] ~~确认睡眠风暴来源~~ → 已定位（2026-08-27），见文末「P4 调查结论」。
- [x] ~~审视 `dumpPstate` 在睡眠路径中逐次 `IOLog` + rdmsr 的锁安全性；
      考虑睡眠路径去掉 IOLog、或失败时不 panic 只降级。~~ → 已在 0.7.4 实施，见下节。
- [x] ~~唤醒路径 3 次 `mp_rendezvous`（setCPBState/writePstate/applyPowerControl）
      合并为 1 次~~ → 已在 0.7.4 实施（新增 `restoreWakeState`），
      同时降级了唤醒后重跑 init 块里的 2 个 panic。
- [ ] 若 panic 复现且栈相同，可实锤该路径；若栈不同，对比新 panic 再定论。
      **复现时先看 kext 版本：≥0.7.4 说明修复后仍复发，=0.7.2 说明还没换装新 kext。**

## 修复记录：0.7.4（2026-08-26）

针对本次 panic 实施了 P0+P1+P3（改动均在 `AMDRyzenCPUPowerManagement.cpp`，
P3 涉及 `AMDRyzenCPUPowerManagement.hpp` 新增方法声明）：

- **P0**：`setPowerState`/`setCPBState`/`dumpPstate` 中的全部 `IOLog` 改为仅
  `-amdpdbg` boot-arg 开启（`ADDPR(debugEnabled)`）时输出 —— 移除电源回调路径上
  的全局日志自旋锁竞争。
- **P1**：电源管理路径与定时器路径上的 `panic()` 全部改为容错降级：
  - `dumpPstate`：rdmsr 失败时该项存 0（恢复时自动跳过无效项）；
  - `setCPBState`：读 HWCR 失败直接返回；
  - `getCPBState`：失败时返回上次已知状态 `sleepState.cpb`；
  - `updateClockSpeed` / `updateInstructionDelta`：读 MSR 失败跳过本次采样。
- **P3**：新增 `restoreWakeState()`，把唤醒路径原来的 3 次 `mp_rendezvous`
  （`setCPBState`/`writePstate`/`applyPowerControl`）合并为 1 次全核同步；
  `setPowerState(1)` 改调它。原三个函数保留（`start()` 和用户态客户端仍在用）。
  另外，唤醒后定时器重跑 init 块（`stopWorkLoop` 会重置 `serviceInitialized`）
  里的 2 个 `panic("wtf?")`（读 HWCR、读 APERF/MPERF 失败）降级为跳过本轮、
  下个定时器周期重试。
- 版本号 0.7.2 → 0.7.4（`project.pbxproj` 两个 build config 均改）。

**新构建产物**：`docs/incidents/AMDRyzenCPUPowerManagement-0.7.4.kext`
（UUID `47D7304E-80FC-3045-9C94-92D6FDF523F5`，自带符号表可直接 atos）。
旧版（出问题的）UUID 为 `6B3B246C-C495-3555-8074-041365033CFC`。

**安装方法**（需手动，boot 关键操作未代执行）：

```bash
sudo diskutil mount disk0s1
# 备份旧 kext 后替换：
sudo cp -R /Volumes/EFI/EFI/OC/Kexts/AMDRyzenCPUPowerManagement.kext ~/AMDRyzenCPUPowerManagement-0.7.2.bak.kext
sudo rm -rf /Volumes/EFI/EFI/OC/Kexts/AMDRyzenCPUPowerManagement.kext
sudo cp -R docs/incidents/AMDRyzenCPUPowerManagement-0.7.4.kext /Volumes/EFI/EFI/OC/Kexts/AMDRyzenCPUPowerManagement.kext
# 重启后验证版本：
kmutil showloaded --list-only | grep spinach
```

---

## P4 调查结论（2026-08-27）：GP17/XHC 唤醒源定位

**拓扑**：`GP17` 是 PCI 桥（`\_SB.PCI0.GP17`），其下游同时挂着
**iGPU（NootedRed，即频繁 gpuRestart 的那块）**、HD 音频、ACP、以及
**XHC0（`\_SB.PCI0.GP17.XHC0`，PCI 桥下 0,3）和 XHC1（0,4）**两个 USB 控制器。
唤醒串 "due to GP17 XHC0 XHC1" 是整条 GP17 电源域的唤醒链。

**排除项**：

- XHC0 **没有挂任何设备**却也出现在唤醒串里 → 不是某个 USB 外设抖信号，
  而是控制器/桥自身的伪唤醒（GPE/PME 层面）。
- XHC1 三个设备（Intel BT 0x8087:0029、SIGMACHIP 鼠标 0x1c4f:0048、
  SONiX 键盘 0x0c45:768a）启动后无反复重枚举 → 不是设备掉线循环。
- USB 端口映射（MyDevicesFake.kext，config 里唯一启用的映射）正确：
  PRT3 Intel BT 已标 internal(255)。USBMap.kext / MyDevicesFakeTahoe.kext 未启用。
- SSDT-USBX.aml 存在于 EFI/OC/ACPI 但**未启用**；无 GPRW/UPRW/USBW 类唤醒抑制补丁。
  config.plist 里有一条 `MWAK→XWAK` 重命名补丁（历史遗留的 _WAK 处理）。

**形态**：每分钟级固定节律——DarkWake 持续 ~45 秒（acwakelinger）→ 睡 ~40 秒 →
立刻再被 GP17/XHC 唤醒，全天候循环（两天内 4000+ 次）。是两次 panic 的共同背景。

**修复选项**（均需改 EFI + 重启）：

1. （推荐，最小改动）config.plist `DeviceProperties → Add` 给
   `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x3)`（XHC0）和 `Pci(0x0,0x4)`（XHC1）加
   `acpi-wake-type = <00>`（Data）。效果同 USBWakeFixup.kext：关掉 USB 控制器的
   ACPI 唤醒能力。**副作用：USB 键盘鼠标无法再唤醒机器，只能用电源键**。
2. SSDT-GPRW 方案：patch XHC0/XHC1 的 `_PRW` 使 S3 下不唤醒（需 iasl 反编译 DSDT，
   本机暂无 iasl）。更精细但工作量大。
3. 顺手启用已存在的 SSDT-USBX.aml（改善 USB 电源属性，对睡眠质量有益，单独不解决风暴）。

**实施记录（2026-08-27）**：采用方案 1。已在 `EFI/OC/config.plist` 的
`DeviceProperties → Add` 写入 `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x3)`（XHC0）与
`PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x4)`（XHC1）各一条 `acpi-wake-type = <00>`；
原配置备份在 `EFI/OC/config.plist.bak-20260827`。**重启后验证**：
`pmset -g log | grep "due to"` 统计中 GP17 XHC0 XHC1 应不再出现；
USB 键鼠唤醒功能随之失效（预期行为，电源键唤醒）。
回滚方法：把备份覆盖回 config.plist 即可。

---

## 修复未生效：根因复盘（2026-08-31）

**事实**：panic 复现，`docs/incidents/AMDRyzenCPUPowerManagement-0.7.4.kext`
（UUID `47D7304E-...-F523F5`）确认已部署在 `/Volumes/EFI/EFI/OC/Kexts/`。
栈形与 0.7.2 同构（仅 kernelcache slide 不同导致 XNU 内核帧偏移），关键 kext
内帧 `setPowerState` / `dumpPstate` 仍在调用栈上 —— **0.7.4 的 P0/P1/P3 修复
未触及本次 panic 的真实触发点**。

### 真正的根因

**`setPowerState(0)` 路径上的同步 `rdmsr_carefully` 间接抢 `cpu_lock`，
与 sleep 风暴叠加形成活锁**。

1. `setPowerState(0)` 在 IOResources 持有方锁中执行，且 IOResources 的 PM 回调
   不允许睡眠 → 不可重入。
2. 当前线程（CPU 0 的 kernel_task）调 `getCPBState()` → `read_msr(kMSR_HWCR, ...)`
   → `rdmsr_carefully`（XNU 的 `_wrmsr_carefully` 实现）。该函数内部会在
   **当前 CPU 上获取 `cpu_data_t::cpu_lock`**（preempt/I/O 同步屏障）。
3. 紧接 `dumpPstate(sleepState.pstate)` 循环 8 次同样动作 —— 8 次 `cpu_lock`
   获取/释放都在 sleep 路径上。
4. **与此同时**，PM 框架已对其他核发起 sleep 序列，部分 CPU 正在拿同一个
   `cpu_lock` 来响应 `pmCPU` 的 stop / disarm 信号。
5. 当任一核慢半拍卡在 CPU 数据结构收敛上，CPU 0 的 `rdmsr_carefully` 自旋
   等 `cpu_lock` 永远不会达成 —— 16 核 `mp_kdp_enter()` 发 NMI 仅收到 1 ack
   （其他核都在等同一把锁，NMI 也进不去）。
6. spinlock 死锁就是 panic 头里 `Ticket spinlock[…] timeout @lock_ticket.c:371`。

### 为什么 0.7.4 没修到这个点

| 0.7.4 改的地方 | 实际影响 |
|---|---|
| 把 `setPowerState` 里 `IOLog` 改为 `-amdpdbg` 才输出 | ✅ 移除 console 自旋锁竞争，但 panic 头里 ticket 锁不是 console 锁 —— **对本次 panic 无效** |
| `dumpPstate` 失败时存 0、不 panic | ✅ 不在 panic 路径上，但 **失败的 rdmsr_carefully 仍会拿 cpu_lock 再释放** —— 锁竞争仍存在 |
| 唤醒路径合并 mp_rendezvous | ✅ 减少 wake 路径总中断时间，**但本 panic 是 sleep 路径，且发生在 wake 合并段之前** |

也就是说：**panic 的锁不在 IOLog，也不在 mp_rendezvous，而在 sleep 路径上
同步调 `rdmsr_carefully` 触发的 `cpu_lock` 抢占**。0.7.4 的修复方向选错了锁。

### 与 sleep 风暴的耦合

P4 已用 `acpi-wake-type=<00>` 关闭 XHC0/XHC1 的 ACPI 唤醒，使 DarkWake 频率从
每 ~45 s 降到更长周期。但即便频率下降，**只要 `setPowerState(0)` 同步读 MSR
这个根因还在，每次 sleep 都是一次"与 16 核抢 cpu_lock"的赌博**。
sleep 风暴只是放大命中概率的乘数，不是根因。

### 下一步（待实施）

完整修复计划见
[`docs/Task/Active/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md`](../../Task/Active/AMDRyzenCPUPM_SLEEP_RDMSR_FIX_PLAN.md)。

核心方向（不重叠于 0.7.4）：

1. **删除 `setPowerState(0)` 中的冗余 `dumpPstate(sleepState.pstate)` 调用**：
   wake 路径上 `initWorkLoop` 的首轮已经把 `dumpPstate(nullptr)` 跑过一次，
   `setPowerState(1)` 又调 `restoreWakeState()` 把 sleepState.pstate 写回 MSR，
   save-then-restore 是 no-op（中间无用户态客户端改 P-state）。完全可省。
2. **`setPowerState` 中所有 MSR 读写用 `mp_rendezvous_no_intrs` 包起来**：
   sleep 路径上的 CPU 全部进入 rendezvous 屏障等待，不会在半路被 sleep 序列
   拖走，避免局部状态机半路退出。
3. **额外兜底**：`getCPBState()` 失败时已经返回 `sleepState.cpb`，
   不要再调 `read_msr`；这一项 0.7.4 已做，仅需复核。

### 仍存在的不确定性

- 当前 kext 二进制为 0.7.4 公开构建，自带符号表；本次未在现场重做 atos。
  "栈内 kext 帧 = `setPowerState` / `dumpPstate`" 是基于 0.7.2 已知符号化结果
  + 0.7.4 偏移范围推算（kext 段基址一致、相对位移仍在 dumpPstate/写 P-state MSR
  入口附近）。行号级证据需要在 panic 现场拉 EFI 分区 kext 重做 atos。
- 不排除 `cpu_lock` 之外其他 ticket 锁（如 PM 私有锁、IOPL 内部锁）的可能，
  但 panic 头里只给出一个 spinlock 地址，且栈型高度吻合 sleep 回调路径，
  概率上 `cpu_lock` 占主导。
