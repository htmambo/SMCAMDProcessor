# 2026-08-27 NVMe Panic 事件档案

> 关联事件：`2026-08-26-kernel-panic.md`（spinlock 死锁，AMDRyzenCPUPowerManagement）。
> 本次是**不同的根因**，但诱因环境相同（睡眠风暴 + Standby 切换）。

## 结论摘要

- **Panic 发生时间 ≈ 2026-08-27 06:27**（在 06:27:05 一次 "DarkWake from **Standby**"
  期间/紧随后；pmset 日志在此后中断。panic 头里的 uptime 70314s 是 mach_absolute
  时间、不含睡眠时长，不能按墙钟直接换算）。
- 机器 panic 后停在现场未自动重启，约 **09:00** 才重新引导（`last` 无 shutdown 记录）。
- 当时 kext 已换装 **0.7.4**（`kmutil` 确认 UUID 47D7304E-… 与修复构建一致），
  **08-26 的 spinlock 死锁没有复发** —— 本次调用栈里完全没有 AMDRyzenCPUPowerManagement。

## Panic 内容

```
panic(cpu 0): nvme: "3rd party NVMe controller. Command timeout. Write.
  MODEL=Lexar SSD NM610PRO 2TB FW=9442 ... CSTS=0x1" @IONVMeController.cpp:5848
Panicked task: kernel_task
Kext in backtrace: com.apple.iokit.IONVMeFamily(2.1)
```

即：系统盘之外的 **Lexar NM610PRO 2TB（disk0，Windows + 数据分区）**在唤醒过程中
一条写命令超时，IONVMeFamily 主动 panic。macOS 对内置 NVMe 的不可恢复错误
一律 panic 处理，哪怕出事的不是启动盘。

## 现场环境（与 08-26 事件的共同诱因）

- 睡眠/唤醒风暴仍在继续：GP17 XHC0 XHC1 每 ~1 分钟 DarkWake 一次；
  panic 前刚从 Standby 唤醒（WakeTime 10.975s，明显偏长）。
- 每次 Standby/唤醒循环 = NVMe 上下电一次，对无 DRAM 的入门级盘是高危操作。

## Lexar NM610PRO 健康数据（smartctl，2026-08-27）

- Critical Warning 0x00，介质错误 0，备用块 100%，寿命已用 24%
- 温度正常（41°C）；已写 98.8TB / 已读 88.3TB
- **Unsafe Shutdowns: 562**（与该机长期 panic/硬挂历史吻合）
- Error Log Entries: 69（`smartctl -l error` 未解析出明细）
- 固件 FW=9442

## 排查/修复方向

- [ ] **更新 Lexar 固件**（FW=9442，需用 Windows 下的 Lexar 工具）——
      入门无 DRAM 盘的固件 bug 是这类 timeout 的常见原因。
- [ ] 减少 NVMe 上下电频率：
      - `sudo pmset -a disksleep 0`（当前 10 分钟）；
      - 评估关闭 standby（`sudo pmset -a standby 0`）减少深度电源切换。
- [ ] 治理 GP17/XHC 睡眠风暴（见 08-26 档案 P4）——两起 panic 的共同背景，
      优先级提高。
- [ ] 若再复发：考虑 PCIe 链路层面（ASPM，OC 可对 NVMe 设备设
      `pci-aspm-default=0` 的 device-property），或怀疑盘体本身。
- [ ] 数据保护：该盘已 562 次不安全关机，重要数据确认有备份。

## 原始证据

- panic 原始文件：本目录 `Kernel-2026-08-27-090106.panic`
