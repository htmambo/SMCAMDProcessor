#!/usr/bin/env bash
#
# SMCAMDProcessor 本地/CI 一键打包脚本
#
# 流程:依赖准备 -> 4 个 scheme 构建 -> 清理 -> 扁平化归档 -> zip -> 产物摘要
#
# 用法:./Scripts/build.sh [选项]
# 详见 --help
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# macOS zip 默认会写入 __MACOSX 资源叉目录,导出该变量关闭该行为
export COPYFILE_DISABLE=1

PROJECT="SMCAMDProcessor.xcodeproj"
CONFIGURATION="Release"
ARCHS="x86_64"
DERIVED_DATA="$ROOT/build/DerivedData"
OUT_DIR="$ROOT/build/Release"
ZIP_FILE="$ROOT/build/Release.zip"
SKIP_DEPS=0
VSMC_REF="master"
DO_CLEAN=0
DO_ZIP=1
VERBOSE=0

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法:./Scripts/build.sh [选项]

选项:
  --configuration <Release|Debug>  构建配置 (默认: Release)
  --arch <arch[,arch...]>         目标架构,逗号分隔 (默认: x86_64)
  --derived-data <dir>             DerivedData 路径 (默认: build/DerivedData)
  --out <dir>                      归档目录 (默认: build/Release)
  --zip <file>                     压缩包路径 (默认: build/Release.zip)
  --vsmc-ref <ref>                 VirtualSMC 分支/tag/commit (默认: master)
  --skip-deps                      跳过依赖准备
  --clean                          构建前清空 DerivedData 与输出目录
  --no-zip                         只产出归档目录,不打包
  --verbose                        输出完整 xcodebuild 日志
  -h, --help                       显示本帮助

产物布局(build/Release.zip 解压后根目录):
  AMDRyzenCPUPowerManagement.kext/
  SMCAMDProcessor.kext/
  AMD Power Gadget.app/     (内含 Contents/Library/LoginItems/APGLaunchHelper.app)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --configuration) CONFIGURATION="${2:?--configuration 需要参数}"; shift 2 ;;
        --arch)          ARCHS="${2:?--arch 需要参数}"; shift 2 ;;
        --derived-data)  DERIVED_DATA="${2:?--derived-data 需要参数}"; shift 2 ;;
        --out)           OUT_DIR="${2:?--out 需要参数}"; shift 2 ;;
        --zip)           ZIP_FILE="${2:?--zip 需要参数}"; shift 2 ;;
        --vsmc-ref)      VSMC_REF="${2:?--vsmc-ref 需要参数}"; shift 2 ;;
        --with-lilu)     die "--with-lilu 已移除:Lilu.kext 现为必需依赖" ;;
        --skip-deps)     SKIP_DEPS=1; shift ;;
        --clean)         DO_CLEAN=1; shift ;;
        --no-zip)        DO_ZIP=0; shift ;;
        --verbose)       VERBOSE=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "未知参数: $1 (用 --help 查看用法)" ;;
    esac
done

case "$CONFIGURATION" in
    Release|Debug) ;;
    *) die "不支持的构建配置: $CONFIGURATION" ;;
esac

# 统一为绝对路径:后续流程存在 cd,相对路径会解析到错误位置
abspath() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "$ROOT/$1" ;;
    esac
}
DERIVED_DATA="$(abspath "$DERIVED_DATA")"
OUT_DIR="$(abspath "$OUT_DIR")"
ZIP_FILE="$(abspath "$ZIP_FILE")"

# rm -rf 目标护栏:禁止空路径与根目录
for p in "$DERIVED_DATA" "$OUT_DIR" "$ZIP_FILE"; do
    case "$p" in
        ""|"/") die "拒绝使用危险路径: '$p'" ;;
    esac
done

# ---------------------------------------------------------------- 前置检查 --
command -v xcodebuild >/dev/null 2>&1 || die "未找到 xcodebuild,请先安装 Xcode 并执行 xcode-select"
command -v git        >/dev/null 2>&1 || die "未找到 git"
command -v ditto      >/dev/null 2>&1 || die "未找到 ditto"
[ -f "$ROOT/$PROJECT/project.pbxproj" ] || die "工程文件缺失: $PROJECT"

if [ "$DO_ZIP" -eq 1 ]; then
    command -v zip >/dev/null 2>&1 || die "未找到 zip"
fi

# 归档架构参数
ARCH_ARGS=()
OLDIFS=$IFS
IFS=','
for a in $ARCHS; do
    if [ -n "$a" ]; then
        ARCH_ARGS+=("-arch" "$a")
    fi
done
IFS=$OLDIFS
[ ${#ARCH_ARGS[@]} -gt 0 ] || die "--arch 不能为空"

PRODUCTS_DIR="$DERIVED_DATA/Build/Products/$CONFIGURATION"
LOG_FILE="$ROOT/build/build.log"

# ---------------------------------------------------------------- 依赖准备 --
prepare_mackernel_sdk() {
    if [ -f "$ROOT/MacKernelSDK/Library/universal/libkmod.a" ]; then
        log "MacKernelSDK 已就绪"
        return 0
    fi
    log "初始化 MacKernelSDK 子模块"
    git -C "$ROOT" submodule update --init --recursive \
        || die "MacKernelSDK 子模块初始化失败(网络问题?可手动执行 git submodule update --init --recursive)"
}

prepare_virtual_smc() {
    if [ -f "$ROOT/VirtualSMC/VirtualSMCSDK/kern_vsmcapi.hpp" ]; then
        log "VirtualSMC 已就绪"
        return 0
    fi
    log "克隆 VirtualSMC (ref: $VSMC_REF)"
    rm -rf "$ROOT/VirtualSMC"
    if ! git clone --depth 1 --branch "$VSMC_REF" \
            https://github.com/acidanthera/VirtualSMC.git "$ROOT/VirtualSMC" >/dev/null 2>&1; then
        # --branch 不接受 commit SHA,回退到完整克隆后再 checkout
        warn "--branch 克隆失败,尝试完整克隆后 checkout $VSMC_REF"
        rm -rf "$ROOT/VirtualSMC"
        git clone https://github.com/acidanthera/VirtualSMC.git "$ROOT/VirtualSMC" >/dev/null 2>&1 \
            || die "VirtualSMC 克隆失败,请检查网络或改用 --vsmc-ref 指定分支/tag"
        git -C "$ROOT/VirtualSMC" checkout "$VSMC_REF" \
            || die "VirtualSMC checkout $VSMC_REF 失败"
    fi
    [ -f "$ROOT/VirtualSMC/VirtualSMCSDK/kern_vsmcapi.hpp" ] \
        || die "VirtualSMC 克隆完成但未找到 VirtualSMCSDK/kern_vsmcapi.hpp,ref 可能不兼容"
}

prepare_lilu() {
    # Lilu.kext 提供 Headers/kern_efi.hpp 等内核头,kext 编译强依赖。
    # 等价于官方 bootstrap.sh 的默认(prebuilt)模式,但不 eval 远程脚本。
    if [ -f "$ROOT/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ]; then
        log "Lilu.kext 已就绪"
        return 0
    fi
    local tag url tmp manifest
    manifest="$(curl -Lfs https://api.github.com/repos/acidanthera/Lilu/releases/latest)" \
        || die "获取 Lilu 最新版本号失败(GitHub API 不可达或触发限流?)"
    tag="$(printf '%s' "$manifest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')"
    [ -n "$tag" ] || die "未能解析 Lilu 最新版本号"

    url="https://github.com/acidanthera/Lilu/releases/download/${tag}/Lilu-${tag}-DEBUG.zip"
    tmp="$(mktemp -d)" || die "创建临时目录失败"
    log "下载 Lilu $tag"
    curl -Lfs -o "$tmp/lilu.zip" "$url" || { rm -rf "$tmp"; die "Lilu 下载失败: $url"; }
    unzip -q "$tmp/lilu.zip" -d "$tmp" || { rm -rf "$tmp"; die "Lilu 压缩包解压失败"; }
    [ -d "$tmp/Lilu.kext" ] || { rm -rf "$tmp"; die "Lilu 压缩包中未找到 Lilu.kext"; }

    rm -rf "$ROOT/Lilu.kext"
    mv "$tmp/Lilu.kext" "$ROOT/Lilu.kext"
    rm -rf "$tmp"
    [ -f "$ROOT/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ] \
        || die "Lilu.kext 结构异常:缺少 Contents/Resources/Headers/kern_api.hpp"
}

if [ "$SKIP_DEPS" -eq 1 ]; then
    warn "已跳过依赖准备"
else
    prepare_mackernel_sdk
    prepare_lilu
    prepare_virtual_smc
fi

# -------------------------------------------------------------------- 构建 --
build_scheme() {
    local scheme="$1"
    log "构建 $scheme ($CONFIGURATION, ${ARCHS})"
    mkdir -p "$(dirname "$LOG_FILE")"
    if [ "$VERBOSE" -eq 1 ]; then
        xcodebuild -scheme "$scheme" -jobs 1 "${ARCH_ARGS[@]}" \
            -configuration "$CONFIGURATION" -derivedDataPath "$DERIVED_DATA" \
            2>&1 | tee "$LOG_FILE"
    else
        if ! xcodebuild -scheme "$scheme" -jobs 1 "${ARCH_ARGS[@]}" \
                -configuration "$CONFIGURATION" -derivedDataPath "$DERIVED_DATA" \
                >"$LOG_FILE" 2>&1; then
            printf '\033[1;31m[error]\033[0m %s 构建失败,日志尾部:\n' "$scheme" >&2
            tail -n 60 "$LOG_FILE" >&2
            exit 1
        fi
    fi
}

if [ "$DO_CLEAN" -eq 1 ]; then
    log "清理 DerivedData 与输出目录"
    rm -rf "$DERIVED_DATA" "$OUT_DIR" "$ZIP_FILE"
fi

# 顺序要求:APGLaunchHelper 必须先于 AMD Power Gadget 构建,
# 后者的 CopyFiles 阶段从 BUILT_PRODUCTS_DIR 取 APGLaunchHelper.app
build_scheme "AMDRyzenCPUPowerManagement"
build_scheme "APGLaunchHelper"
build_scheme "AMD Power Gadget"
build_scheme "SMCAMDProcessor"

# -------------------------------------------------------------------- 清理 --
log "清理中间产物"
rm -rf "$PRODUCTS_DIR"/*.dSYM "$PRODUCTS_DIR"/*.swiftmodule
# 只保留内嵌于 AMD Power Gadget.app 的 helper,删除顶层独立副本
rm -rf "$PRODUCTS_DIR/APGLaunchHelper.app"

# -------------------------------------------------------------------- 归档 --
log "归档到 $OUT_DIR"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

for product in "AMDRyzenCPUPowerManagement.kext" "SMCAMDProcessor.kext" "AMD Power Gadget.app"; do
    [ -e "$PRODUCTS_DIR/$product" ] || die "构建产物缺失: $PRODUCTS_DIR/$product"
    ditto "$PRODUCTS_DIR/$product" "$OUT_DIR/$product"
done

# 内嵌 helper 校验
[ -d "$OUT_DIR/AMD Power Gadget.app/Contents/Library/LoginItems/APGLaunchHelper.app" ] \
    || die "AMD Power Gadget.app 内未找到 APGLaunchHelper.app"

if [ "$DO_ZIP" -eq 1 ]; then
    log "打包 $ZIP_FILE"
    mkdir -p "$(dirname "$ZIP_FILE")"
    rm -f "$ZIP_FILE"
    # 在 staging 目录内打包,保证压缩包根目录就是产物本身
    ( cd "$OUT_DIR" && zip -r -q -X "$ZIP_FILE" . -x '*.DS_Store' ) \
        || die "打包失败: $ZIP_FILE"
fi

# -------------------------------------------------------------------- 摘要 --
plist_get() {
    /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || echo "N/A"
}

printf '\n'
log "产物摘要"
printf '  %s\n' "--------------------------------------------------------------"

for bundle in "AMDRyzenCPUPowerManagement.kext" "SMCAMDProcessor.kext"; do
    path="$OUT_DIR/$bundle"
    plist="$path/Contents/Info.plist"
    bin="$path/Contents/MacOS/${bundle%.kext}"
    printf '  %s\n' "$bundle"
    printf '    CFBundleVersion          : %s\n' "$(plist_get "$plist" CFBundleVersion)"
    printf '    CFBundleShortVersionString: %s\n' "$(plist_get "$plist" CFBundleShortVersionString)"
    printf '    架构                     : %s\n' "$(lipo -archs "$bin" 2>/dev/null || echo 'N/A')"
    printf '    签名                     : %s\n' \
        "$(codesign -dv "$path" 2>&1 | sed -n 's/^Signature=//p' | head -1)"
    printf '    依赖                     : %s\n' \
        "$(plist_get "$plist" OSBundleLibraries | tr '\n' ' ')"
done

for app in "AMD Power Gadget.app"; do
    path="$OUT_DIR/$app"
    plist="$path/Contents/Info.plist"
    printf '  %s\n' "$app"
    printf '    CFBundleVersion          : %s\n' "$(plist_get "$plist" CFBundleVersion)"
    printf '    CFBundleShortVersionString: %s\n' "$(plist_get "$plist" CFBundleShortVersionString)"
    printf '    内嵌 helper              : %s\n' \
        "$([ -d "$path/Contents/Library/LoginItems/APGLaunchHelper.app" ] && echo 'APGLaunchHelper.app (OK)' || echo '缺失')"
done

printf '  %s\n' "--------------------------------------------------------------"

if [ "$DO_ZIP" -eq 1 ]; then
    printf '  压缩包: %s (%s)\n' "$ZIP_FILE" "$(du -h "$ZIP_FILE" | cut -f1 | tr -d ' ')"
    printf '  内容:\n'
    unzip -l "$ZIP_FILE" | sed -n '4,$p' | sed 's/^/    /'
fi

printf '\n'
log "完成"
