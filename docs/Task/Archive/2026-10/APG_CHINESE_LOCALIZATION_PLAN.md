# AMD Power Gadget 中文语言包（本地化）实施计划

**Status**: ✅ Completed (completion time: 2026-10-23)
**Created**: 2026-10-23

## 任务目标与背景

AMD Power Gadget 应用目前全英文，无本地化基础设施（0 处 `NSLocalizedString`，仅 `Base.lproj/Main.storyboard`，`knownRegions` 只有 `en` + `Base`）。目标：添加简体中文语言包，系统语言为中文时应用自动显示中文，其他语言环境回退英文。

翻译要求：使用规范的计算机类专业用语；CPU 领域专有名词（P-State、SpeedShift、SMC、IPC、CCX 等）保留英文不译。

## 现状分析（调查结论）

- 界面技术栈：Storyboard 驱动（AppKit），`NSMainStoryboardFile = Main`；Swift 代码仅做 IBOutlet 绑定与动态赋值。
- 待翻译字符串规模：Storyboard 约 60–70 条（其中约一半为系统标准菜单项，系统自动本地化，跳过）；Swift 代码约 50 条（alert、状态栏菜单、动态格式化文本），涉及约 6–8 个文件。
- 无按语言分支的逻辑需要清理；storyboard 内有一条遗留垃圾文本（`Family: 12 ddd deded...`），翻译时顺手清理。

## 任务拆解

- [x] 1. 工程配置：`project.pbxproj` 的 `knownRegions` 加入 `zh-Hans`；新建 `zh-Hans.lproj` 目录 — 通过 `xcodebuild -exportLocalizations` / `-importLocalizations` 官方流程自动完成，未手写 pbxproj
- [x] 2. Storyboard 本地化：`zh-Hans.lproj/Main.strings`（110 条，55 译 55 保留；系统标准菜单项 target=source 交给 AppKit 运行时本地化）；垃圾文本已清理（清空 title，运行时由 setupOverview 覆写）
- [x] 3. 代码字符串本地化：`zh-Hans.lproj/Localizable.strings`（45 条，40 译 5 保留）；7 个 Swift 文件硬编码字符串改为 `NSLocalizedString`（key=英文原文，无 en.lproj，英文走 key 回退）；格式化串 `String(format:)` 化；顺手修正 `Ghz→GHz`、`Mhz→MHz`、`Storage:` 的 `%d→%@`（storageGB 为 String 的真 bug）
- [x] 4. 构建验证：`xcodebuild` Release 构建通过；`.app` 内含 `zh-Hans.lproj/{Main,Localizable,InfoPlist}.strings`；语言选择验证 zh-Hans→中文、ja/fr→英文回退；`plutil -lint` 全部 OK
- [x] 5. 外部审查：plan 审查通过（3 个前置条件均落实）；code 审查（review_code 两次超时，改走 chat 通道）发现 UInt64→`%d`/`%X` 32 位截断隐患（原代码既有写法），已对 CPUID 及 SMC 芯片 ID 参数加 `UInt32()` 强转（CPUID 架构上即 32 位），复建通过

## 验收结果

- 构建通过，无新增 warning/error。
- 中文（zh-Hans）下窗口、菜单、按钮、alert 显示中文；英文及其他语言回退英文，与改动前一致。
- 占位符校验脚本：Main.strings 110/110、Localizable.strings 45/45 说明符序列一致。
- 遗留：未 git commit（待用户指示）；归档（docs/Task/Archive/2026-10/）随首次提交时一并处理。

## 经验总结

- `xcodebuild -exportLocalizations` + 翻译 XLIFF + `-importLocalizations` 是零手写 pbxproj 的安全路径，导入自动建 variant group 并挂 Resources phase。
- 系统标准菜单项（About/Hide/Quit/Services/Window/Edit/Help）在 strings 文件中必须 target=source，否则覆盖 AppKit 的规范本地化。
- Swift `String(format:)` 中 `UInt64` 直接配 `%d`/`%X` 不会崩（ABI 64 位传参）但会截断，32 位语义值应显式 `UInt32()` 强转。
- 菜单栏 7px 微型缩写标签（TEM/PWR）不宜翻译，会破坏布局。

## 关键翻译术语表（约定）

| 英文 | 中文 | 说明 |
|---|---|---|
| Fan Control | 风扇控制 | |
| Speed Steps | 速度档位 | AMD 语境下指性能状态档位 |
| SpeedShift | SpeedShift | 专有名词保留 |
| P-State | P-State | 保留 |
| Effective Frequency | 有效频率 | |
| Frequency | 频率 | |
| Temperature | 温度 | |
| Power | 功耗 | 功率计语境 |
| Demand | 占用率 | CPU 需求 |
| Low Power State | 低功耗状态 | |
| Advanced Options | 高级选项 | |
| Apply / Revert | 应用 / 还原 | |
| Import / Export | 导入 / 导出 | |
| Take Off | 起飞 | 风扇全速档位的形象说法 |
| SMC Fans | SMC 风扇 | SMC 保留 |
| motherboard | 主板 | |
| Not yet supported | 尚不支持 | |
| Could not found driver... | 找不到驱动… | |
| Launch in menu bar at login | 登录时在菜单栏启动 | |
| Show metrics in menu bar | 在菜单栏显示指标 | |
| Quit and Download | 退出并下载 | |
| Close without saving | 不保存并关闭 | |

## 预期效果与验收标准

- 构建通过，无新增 warning 报错。
- 系统语言为简体中文时：窗口、菜单、按钮、alert 全部显示中文。
- 系统语言为英文或其他语言时：显示与改动前一致的英文。
- 格式化字符串（频率/温度/功耗）在中文下输出正常，占位符不丢失。

## 风险与缓解

- 风险：storyboard 后续改版后 `Main.strings` 不同步。缓解：采用 strings 文件方式而非 duplicate storyboard，键值为对象 ID，Xcode 改版时可重新生成 diff。
- 风险：格式化占位符被翻译破坏。缓解：所有格式串在 `Localizable.strings` 中保留 `%d`/`%.2f`，翻译后人工核对。
- 风险：pbxproj 手工编辑出错导致工程无法打开。缓解：编辑后立刻 `xcodebuild` 验证。

## 实施顺序与依赖

1 → 2 → 3 可部分并行（2 与 3 无依赖），4 依赖全部，5 在 4 通过后进行。
