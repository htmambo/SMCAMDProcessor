# BUILD_SCRIPT — 本地/CI 一键打包脚本

**Status**: ✅ Completed (completion time: 2026-10-28)
**创建时间**: 2026-10-28
**创建者**: 果农 + Claude Code
**归档位置**: `docs/Task/Archive/2026-10/BUILD_SCRIPT_PLAN.md`

## 一、任务目标与背景

### 目标
新增一个可复现的打包脚本 `Scripts/build.sh`，一键完成「依赖准备 → 4 个 scheme 构建 → 产物清理 → 扁平化归档 → 生成 `build/Release.zip` → 打印产物版本摘要」。

### 背景（现状分析）

当前项目**没有打包脚本**，打包逻辑完全散落在 `.github/workflows/main.yml` 的手写步骤中：

| 现状 | 问题 |
|---|---|
| `xcodebuild` 逐个 scheme 调用 | 本地复现需手工敲 4 条命令再手工 zip |
| `zip -r build/Release.zip build/Build/Products/Release` | 压缩包内含 `build/Build/Products/Release/` 四层冗余路径 |
| `-arch x86_64 -arch ACID32` | `ACID32` 非有效架构（ocbuild 仅提供 clang32 工具链，未注册该 arch），实际产物为 x86_64 单架构 |
| `rm -rf build/Build/Products/Release/APGLaunchHelper*` | 删除独立副本，只保留内嵌于 `AMD Power Gadget.app/Contents/Library/LoginItems/` 的副本（依赖构建顺序的隐式约定） |
| 无依赖检查 | 本地 `MacKernelSDK` 子模块未初始化、`Lilu.kext`、`VirtualSMC` 缺失时直接构建必然失败 |

### 目标产物布局

`build/Release.zip` 解压后根目录直接是 3 个产物：

```
AMDRyzenCPUPowerManagement.kext/
SMCAMDProcessor.kext/
AMD Power Gadget.app/          (内含 Contents/Library/LoginItems/APGLaunchHelper.app)
```

## 二、现状事实核对（已通过读取代码/工程文件确认）

- 工程文件：`SMCAMDProcessor.xcodeproj/project.pbxproj`
- 4 个 target 及产物：
  - `AMDRyzenCPUPowerManagement` → `.kext`，`wtf.spinach.AMDRyzenCPUPowerManagement`，`CURRENT_PROJECT_VERSION = MARKETING_VERSION = 0.7.5`
  - `SMCAMDProcessor` → `.kext`，`wtf.spinach.SMCAMDProcessor`，`1.0.0d1` / `1.0.1`
  - `AMD Power Gadget` → `.app`，`wtf.spinach.AMD-Power-Gadget`，`MARKETING_VERSION = 1.7.2`
  - `APGLaunchHelper` → `.app`，`wtf.spinach.APGLaunchHelper`，沙盒开启，deployment target 11.3
- **无 target 依赖、无 Shell Script 构建阶段**；`AMD Power Gadget` 通过 CopyFiles 阶段（`dstPath = Contents/Library/LoginItems`）内嵌 `APGLaunchHelper.app`，故必须**先构建 APGLaunchHelper**
- 头文件依赖（实跑后修正，详见第八节）：
  - `MacKernelSDK`（git 子模块）→ IOKit / osfmk 等内核头
  - **`Lilu.kext`** → `Headers/kern_efi.hpp`、`Headers/kern_api.hpp`（kext 编译强依赖）
  - `VirtualSMC` 仓库 → `VirtualSMCSDK/kern_vsmcapi.hpp`、`VirtualSMCSDK/AppleSmc.h`
- 签名：kext `CODE_SIGN_STYLE = Manual` 且无 team id → ad-hoc 签名（实测 `Signature=adhoc`）

## 三、任务拆解与状态

| # | 子任务 | 状态 |
|---|---|---|
| 1 | 需求分析 + 外部评审（requirement） | ⚠️ 完成，但外部评审不可用（见第九节） |
| 2 | 实施方案 + 外部评审（plan） | ⚠️ 完成，同上 |
| 3 | 编写 `Scripts/build.sh` | ✅ |
| 4 | 外部代码评审（code）并修复问题 | ⚠️ 外部不可用，改为自审 + 实跑验证（见第九节） |
| 5 | 干净环境实跑验证 | ✅ |
| 6 | 幂等性验证 | ✅ |
| 7 | 归档 + 更新 `docs/Task/README.md` | ✅ |

## 四、实现要点

### `Scripts/build.sh`（唯一新增文件）

**参数**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--configuration <Release\|Debug>` | `Release` | 构建配置 |
| `--arch <arch[,arch...]>` | `x86_64` | 目标架构；不使用无效的 `ACID32` |
| `--derived-data <dir>` | `build/DerivedData` | DerivedData 路径 |
| `--out <dir>` | `build/Release` | 归档 staging 目录 |
| `--zip <file>` | `build/Release.zip` | 压缩包路径 |
| `--vsmc-ref <ref>` | `master` | VirtualSMC 分支/tag/commit |
| `--skip-deps` | 关 | 跳过依赖准备 |
| `--clean` | 关 | 构建前清空 DerivedData 与输出目录 |
| `--no-zip` | 关 | 只产出 staging 目录 |
| `--verbose` | 关 | 透传完整 xcodebuild 日志 |

**执行流程**

1. **前置检查**：`xcodebuild` / `git` / `ditto` / `zip` 可用；工程文件存在
2. **路径归一**：相对路径统一按仓库根解析为绝对路径（避免 `cd` 后相对路径错位）；`rm -rf` 目标护栏拒绝空路径与 `/`
3. **依赖准备**
   - `MacKernelSDK/Library/universal/libkmod.a` 缺失 → `git submodule update --init --recursive`
   - `Lilu.kext/Contents/Resources/Headers/kern_api.hpp` 缺失 → 调 GitHub Releases API 取最新 tag，下载 `Lilu-<tag>-DEBUG.zip` 解压安装（等价于官方 `bootstrap.sh` 的 prebuilt 模式，但**不 eval 远程脚本**）
   - `VirtualSMC/VirtualSMCSDK/kern_vsmcapi.hpp` 缺失 → `git clone --depth 1 -b <ref>`；`--branch` 不接受 commit SHA 时回退为完整克隆 + `checkout`
4. **构建**（顺序固定）：`AMDRyzenCPUPowerManagement` → `APGLaunchHelper` → `AMD Power Gadget` → `SMCAMDProcessor`
   - `APGLaunchHelper` 必须先于 `AMD Power Gadget`：后者的 CopyFiles 阶段从 `BUILT_PRODUCTS_DIR` 取 `APGLaunchHelper.app`
   - 4 次调用共用同一 `-derivedDataPath`，保证 `BUILT_PRODUCTS_DIR` 一致
   - 失败时打印 `build/build.log` 尾部 60 行
5. **清理**：删除 `*.dSYM`、`*.swiftmodule`、顶层独立 `APGLaunchHelper.app`
6. **归档**：`ditto` 拷贝 3 个产物到 staging（保留 bundle 结构与签名），校验内嵌 helper 存在，再在 staging 内 `zip -r -X`（`COPYFILE_DISABLE=1` 防 `__MACOSX`），压缩包根目录即产物
7. **摘要**：打印 kext 版本 / 架构 / 签名类型 / OSBundleLibraries 与 app 版本

## 五、验收结果

| 验收项 | 判定 | 结果 |
|---|---|---|
| 干净环境执行返回 0 | 返回码 0 | ✅ `EXIT=0` |
| 产出 `build/Release.zip` | 存在且非空 | ✅ 5.7 MB / 91 files |
| zip 根目录为 3 个产物 | 无冗余前缀 | ✅ `AMD Power Gadget.app`、`AMDRyzenCPUPowerManagement.kext`、`SMCAMDProcessor.kext` |
| 无 `__MACOSX` 条目 | 计数为 0 | ✅ |
| kext 为有效 x86_64 二进制 | `lipo -archs` | ✅ `x86_64`（`file`: Mach-O 64-bit kext bundle x86_64） |
| 内嵌 helper 存在 | `Contents/Library/LoginItems/APGLaunchHelper.app` | ✅ |
| 顶层独立 helper 不在归档 | 已删除 | ✅ |
| 重复执行不重复克隆 | 幂等 | ✅ 第二次输出「已就绪」，未触发克隆 |
| kext 签名 | ad-hoc | ✅ `Signature=adhoc`（与既有分发形态一致） |
| 相对路径参数 | `--out/--zip` 相对路径解析正确 | ✅ 产物落在仓库根 `build/` 下而非 staging 内部 |

实测产物摘要：

```
AMDRyzenCPUPowerManagement.kext  0.7.5 / 0.7.5  x86_64  adhoc
SMCAMDProcessor.kext             1 / 1.0.1      x86_64  adhoc
AMD Power Gadget.app             10 / 1.7.2     内嵌 APGLaunchHelper.app (OK)
```

## 六、风险评估与缓解（最终态）

| 风险 | 缓解 |
|---|---|
| VirtualSMC `master` 头文件变动导致编译失败 | `--vsmc-ref` 可指定分支/tag/commit；SHA 走完整克隆回退分支 |
| GitHub API 限流导致 Lilu 版本号获取失败 | 明确报错并提示；已存在 `Lilu.kext` 时完全跳过该步骤 |
| 首次构建耗时长 | `-jobs 1` 与 CI 一致；依赖就绪后重复构建为增量，秒级完成 |
| `rm -rf` 误伤 | 空路径与 `/` 护栏；所有目标路径先归一为绝对路径 |
| ad-hoc 签名 kext 在部分环境需额外签名 | 摘要打印签名类型；脚本不改签名策略（保持既有分发形态） |

## 七、实施顺序

需求评审 → 方案评审 → 编写脚本 → 评审/自审 + 修复 → 干净环境实跑 → 幂等验证 → 归档（全部完成）

## 八、关键发现（实施过程中的事实修正）

1. **Lilu.kext 是必需依赖，不是可选**。`AMDRyzenCPUPowerManagement.hpp` 中的 `#include <Headers/kern_efi.hpp>` 经 `HEADER_SEARCH_PATHS` 的 `${PROJECT_DIR}/Lilu.kext/Contents/Resources` 解析到 Lilu 产物的 `Contents/Resources/Headers/kern_efi.hpp`（MacKernelSDK 与 VirtualSMC 均无此文件）。首轮实跑即因此失败，据此把 Lilu 提升为必选依赖并移除 `--with-lilu` 开关。
2. **不 eval 远程脚本**。官方 `Lilu/Scripts/bootstrap.sh` 采用 `eval "$(curl ...)"` 方式安装 prebuilt Lilu；脚本改为直接走 GitHub Releases API + 下载 + 解压，行为等价但避免执行远程代码。
3. **`-arch ACID32` 确认无效**：`xcodebuild -arch ACID32 -showBuildSettings` 显示 `ARCHS = ACID32` 被原样传入，而产物始终是 x86_64 单架构，脚本不再传递该参数。
4. **版本号分散**：`AMDRyzen` 0.7.5、`SMCAMDProcessor` 1.0.0d1/1.0.1（且 `Info.plist` 的 `CFBundleVersion` 硬编码 `1`）、`AMD Power Gadget` 1.7.2、App 内 `ProcessorModel.swift` 的 `latestKnownVersion = "0.7.5"` 硬编码。**本次不在范围内**，脚本只如实打印现状，供发版时核对。

## 九、外部评审意见

### Round 1（requirement）— ❌ 不可用
### Round 2（plan）— ❌ 不可用
### Round 3（code）— ❌ 不可用

**说明**：本次三次调用外部评审 MCP（provider `coding-bridge`）均返回 `API HTTP 429: Token Plan Person monthly quota limit exceeded`；备用 provider `codex`（`mcp__codex__codex`）在本会话未挂载工具，无法调用。按规范 §1.4 第三优先级，改由本地实跑验证 + 自审交叉核对作为替代手段，**本次变更未经外部评审 MCP 评审**。

自审修复记录：
- 相对路径在 `cd "$OUT_DIR"` 后错位 → 全部路径参数归一为绝对路径
- `rm -rf` 目标缺少护栏 → 拒绝空路径与 `/`
- `curl | sed | head -1` 在 `pipefail` 下可能因 SIGPIPE 误判失败 → 先整体取回 JSON 再解析
- `ARCH_ARGS+=` 前的 `[ -n "$a" ] &&` 在末次迭代为空时导致 `set -e` 误退出 → 改为 `if` 块
- 摘要中 kext 二进制路径误用 `*.kext` 文件名 → 改为 `${bundle%.kext}`

## 十、备注 / 经验总结

- 仓库当前的构建链依赖三个外部资产：MacKernelSDK（子模块）、Lilu.kext（预编译）、VirtualSMC（源码克隆）。三者都不在仓库内，缺少任一都会编译失败，这正是 CI 里那段 bootstrap 脚本存在的原因；脚本把它们固化成了显式的依赖准备步骤。
- CI 的 `zip -r build/Release.zip build/Build/Products/Release` 会把完整路径打进压缩包，本脚本改为在 staging 内打包，压缩包根目录即产物。
- 后续可选改进（未纳入本次范围）：把 CI 的手写构建步骤替换为调用本脚本；统一各处版本号；在 `release: published` 时自动挂载 Release 资产。
