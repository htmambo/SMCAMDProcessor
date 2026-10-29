# CI_VERSION_SYNC — CI 改用打包脚本 + 版本号单一来源

**Status**: ✅ Completed (completion time: 2026-10-28)
**创建时间**: 2026-10-28
**创建者**: 果农 + Claude Code
**归档位置**: `docs/Task/Archive/2026-10/CI_VERSION_SYNC_PLAN.md`

## 一、目标与背景

承接 `BUILD_SCRIPT`（`Scripts/build.sh` 已落地并验证通过），本次做两件事：

1. **CI 改为调用统一脚本**：`.github/workflows/main.yml` 里手写的 4 条 `xcodebuild` + `zip` 步骤改为调用 `Scripts/build.sh`，消除「本地脚本与 CI 两套打包逻辑」的分叉，并顺带去掉无效的 `-arch ACID32`、修掉压缩包四层冗余路径、让 Release 页面自动挂载资产。
2. **版本号单一来源**：消除当前 4 个组件版本号各写各的、且部分硬编码在源码里的问题。

### 现状版本号事实

| 位置 | 变更前 | 问题 |
|---|---|---|
| pbxproj `AMDRyzenCPUPowerManagement` target | `CURRENT_PROJECT_VERSION = MARKETING_VERSION = 0.7.5` | 与 App 内硬编码重复 |
| pbxproj `SMCAMDProcessor` target | `CURRENT_PROJECT_VERSION = 1.0.0d1`、`MARKETING_VERSION = 1.0.1`、`MODULE_VERSION = 1.0.0d1` | 三个值互不一致 |
| `SMCAMDProcessor/Info.plist` | `CFBundleVersion = 1`（硬编码） | 与 target 设置脱钩 |
| pbxproj `AMD Power Gadget` target | `MARKETING_VERSION = 1.7.2`、`CURRENT_PROJECT_VERSION = 10` | 保持不变（marketing 号 + build 号，符合惯例） |
| pbxproj `APGLaunchHelper` target | `1` / `1.0` | 一致，不动 |
| `AMD Power Gadget/ProcessorModel.swift` | `latestKnownVersion = "0.7.5"` 硬编码 | 每次 kext 升版都要改源码 |

> `0.7.5` 这个数字必须**同时**出现在 kext 的 `CFBundleVersion` / `CFBundleShortVersionString` / `MODULE_VERSION`（App 通过 user client 读到的就是 `MODULE_VERSION`），任一漂移都会让 App 的版本比较行为异常。

## 二、实际改动

### 1. `.github/workflows/main.yml`

- 删除 `PROJECT_TYPE` / `ACID32` / `JOB_TYPE` 环境变量与 ocbuild / Lilu bootstrap / VirtualSMC clone 手工步骤（`Scripts/build.sh` 已内建这三项依赖准备：MacKernelSDK 子模块、Lilu.kext 预编译包、VirtualSMC 克隆）
- 构建步骤改为 `./Scripts/build.sh --clean`
- artifact 上传路径改为 `build/Release.zip`
- 新增 `release: published` 时用 `gh release upload --clobber` 把 `build/Release.zip` 挂到 Release 页面（`permissions: contents: write` + 步骤级 `GH_TOKEN`）

### 2. 版本号单一来源

- pbxproj **项目级**（`B57D27FC` Debug / `B57D27FD` Release）新增 `AMDRYZEN_KEXT_VERSION = 0.7.5`，作为唯一来源
- `AMDRyzenCPUPowerManagement` target：`CURRENT_PROJECT_VERSION` / `MARKETING_VERSION` 改为 `"$(AMDRYZEN_KEXT_VERSION)"`；`MODULE_VERSION` 保持 `"$(CURRENT_PROJECT_VERSION)"`
- `SMCAMDProcessor` target：`CURRENT_PROJECT_VERSION = MARKETING_VERSION = 1.0.1`，`MODULE_VERSION = "$(CURRENT_PROJECT_VERSION)"`
- `SMCAMDProcessor/Info.plist`：`CFBundleVersion` 由硬编码 `1` 改为 `$(CURRENT_PROJECT_VERSION)`
- `AMD Power Gadget/Info.plist`：新增 `LatestKnownAMDRyzenCPUPowerManagementVersion = $(AMDRYZEN_KEXT_VERSION)`
- `AMD Power Gadget/ProcessorModel.swift`：`latestKnownVersion` 改为闭包读取 Info.plist，缺失或为空时回退硬编码 `"0.7.5"`

### 3. 未纳入范围

- 不改 `AMD Power Gadget` 自身的 1.7.2 / 10、不改 `APGLaunchHelper` 的 1.0
- 不改 kext 行为逻辑、不改签名策略

## 三、验收结果

| 验收项 | 判定 | 结果 |
|---|---|---|
| 工程文件可解析 | `xcodebuild -list` | ✅ 4 个 scheme 正常列出 |
| 构建设置正确展开 | `xcodebuild -showBuildSettings` | ✅ AMDRyzen: `AMDRYZEN_KEXT_VERSION/CURRENT_PROJECT_VERSION/MARKETING_VERSION/MODULE_VERSION = 0.7.5`；SMCAMDProcessor: 三个版本号均为 `1.0.1` |
| `./Scripts/build.sh` 成功 | 返回码 0 | ✅ |
| `AMDRyzenCPUPowerManagement.kext` | `CFBundleVersion = CFBundleShortVersionString = 0.7.5` | ✅ 实读产物 Info.plist |
| kext 上报给 App 的版本 | 二进制内 `MODULE_VERSION` 字符串 | ✅ `strings` 命中 `0.7.5` |
| `SMCAMDProcessor.kext` | `CFBundleVersion = CFBundleShortVersionString = 1.0.1` | ✅ 不再是硬编码 `1` |
| `AMD Power Gadget.app` | Info.plist 含 `LatestKnownAMDRyzenCPUPowerManagementVersion` | ✅ 实读为 `0.7.5` |
| App 版本比较逻辑 | 兜底路径 | ✅ Info.plist 缺失/为空时回退 `"0.7.5"` |
| workflow YAML 有效 | YAML 解析 | ✅ 4 个步骤名称正确 |
| 打包产物结构不变 | zip 根目录 | ✅ 仍为 3 个产物 |

### 实施中的两个技术要点

1. **pbxproj 的 `$(...)` 必须加引号**：直接写 `CURRENT_PROJECT_VERSION = $(AMDRYZEN_KEXT_VERSION);` 会让旧式 plist 解析器报 `missing semicolon in dictionary`，工程直接打不开；改为 `"$(AMDRYZEN_KEXT_VERSION)"` 后正常（Xcode 自身写 `MODULE_VERSION = "$(CURRENT_PROJECT_VERSION)";` 也是这个原因）。
2. **Info.plist 注入会真实展开**：产物 `AMD Power Gadget.app/Contents/Info.plist` 中该 key 实读为 `0.7.5`，说明构建期变量展开生效。

## 四、风险与缓解

| 风险 | 缓解 |
|---|---|
| 改 pbxproj 手误导致工程打不开 | 改完立即 `xcodebuild -list` + 完整重建验证 |
| Info.plist 变量未展开 | 产物 Info.plist 用 PlistBuddy 实读校验 |
| App 读不到新 key 导致版本判断失效 | Swift 侧保留硬编码兜底，且空串也回退 |
| CI 行为变更（去掉 bootstrap） | 脚本已覆盖全部三项依赖准备，本地已实跑同一路径验证 |
| `gh release upload` 需要令牌权限 | `permissions: contents: write` + 步骤级 `GH_TOKEN` |
| Lilu 版本号查询受 GitHub API 限流 | 已有 `Lilu.kext` 时完全跳过该步骤；失败时明确报错 |

## 五、外部评审

沿用上一任务结论：外部评审 MCP（coding-bridge）返回 429 配额超限，codex 未挂载 → 本次同样以**本地实跑验证 + 逐项核对**替代，并在此明示未经外部评审。

## 六、状态

- 需求/方案：✅
- 实施：✅
- 验证：✅
- 归档：✅
