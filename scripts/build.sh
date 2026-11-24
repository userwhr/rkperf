#!/usr/bin/env bash
#
# rkperf 构建脚本
#
#   ./scripts/build.sh              自动判断：aarch64 本机编译，其他架构交叉编译
#   ./scripts/build.sh --native     强制本机编译（必须在 aarch64 上）
#   ./scripts/build.sh --cross      强制交叉编译到 aarch64
#   ./scripts/build.sh --deps       打印依赖说明
#   ./scripts/build.sh --make-sysroot
#                                   在板子上打包交叉编译用的 sysroot
#
# 产物：build/rkperf
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
SPDLOG_VER=1.14.1
SPDLOG_DIR="$BUILD/spdlog-$SPDLOG_VER"

MODE=auto
SYSROOT=${SYSROOT:-$HOME/aarch64-sysroot}
DEPS=${DEPS:-}            # 可选：自备 aarch64 依赖根（include/bpf + lib/libbpf.a）
CROSS_PREFIX=${CROSS_PREFIX:-aarch64-linux-gnu-}

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法:
  ./scripts/build.sh                 自动判断：aarch64 本机编译，其他架构交叉编译
  ./scripts/build.sh --native        强制本机编译（必须在 aarch64 上）
  ./scripts/build.sh --cross         强制交叉编译到 aarch64
  ./scripts/build.sh --deps          打印依赖说明
  ./scripts/build.sh --make-sysroot  在板子上打包交叉编译用的 sysroot

环境变量:
  SYSROOT        aarch64 sysroot 路径，默认 $HOME/aarch64-sysroot
  CROSS_PREFIX   交叉工具链前缀，默认 aarch64-linux-gnu-

产物: build/rkperf
EOF
}

deps() {
    cat <<'EOF'
依赖说明
========

【通用】
  clang        编译 BPF 程序（需要 -target bpf 支持，clang >= 10）
  ld           把 BPF 对象塞进 ELF（binutils）
  curl         下载 spdlog（已有 build/spdlog-1.14.1 时可跳过）
  git          拉代码（如果还没 clone）

【本机编译（在 RK3588 板子上）】
  g++ >= 9
  libbpf-dev   (头文件 /usr/include/bpf/*.h + libbpf.a)
  libelf-dev   (libelf.a)
  zlib1g-dev   (libz.a)
  Ubuntu/Debian: sudo apt install clang g++ make libbpf-dev libelf-dev zlib1g-dev curl

【交叉编译（在 x86_64 上）】
  1) aarch64 交叉工具链
     Ubuntu:  sudo apt install g++-aarch64-linux-gnu
     或下载:  https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
     工具链前缀通过环境变量指定: CROSS_PREFIX=aarch64-linux-gnu-

  2) aarch64 sysroot（板子上的头文件和静态库）
     在板子上执行:  ./scripts/build.sh --make-sysroot
     把生成的 sysroot.tgz 拷到编译机，解压到 $HOME/aarch64-sysroot：
         mkdir -p $HOME/aarch64-sysroot
         tar xzf sysroot.tgz -C $HOME/aarch64-sysroot
     自定义位置:  SYSROOT=/path/to/sysroot ./scripts/build.sh

  3) 目标板上需要 libbpf / libelf / zlib 的静态库（sysroot 里已包含）

spdlog 会自动下载到 build/ 目录，不需要提前安装。
EOF
}

make_sysroot() {
    [ "$(uname -m)" = "aarch64" ] || die "--make-sysroot 必须在板子上运行"
    local out="${1:-$ROOT/sysroot.tgz}"
    out=$(realpath -m "$out")
    say "打包 sysroot -> $out"
    ( cd / && tar czf "$out" \
        usr/include \
        usr/lib/aarch64-linux-gnu/*.a \
        usr/lib/aarch64-linux-gnu/*.o \
        usr/lib/gcc 2>/dev/null )
    say "完成，大小 $(du -h "$out" | cut -f1)"
    echo
    echo "在编译机上执行："
    echo "  scp <board>:$out ."
    echo "  mkdir -p \$HOME/aarch64-sysroot"
    echo "  tar xzf $(basename "$out") -C \$HOME/aarch64-sysroot"
    echo "  ./scripts/build.sh --cross"
}

fetch_spdlog() {
    if [ -d "$SPDLOG_DIR/include" ]; then
        say "spdlog 已存在: $SPDLOG_DIR"
        return
    fi
    command -v curl >/dev/null || die "缺少 curl，无法下载 spdlog"
    mkdir -p "$BUILD"
    say "下载 spdlog $SPDLOG_VER"
    curl -fsSL "https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v$SPDLOG_VER" \
        -o "$BUILD/spdlog.tar.gz"
    tar xzf "$BUILD/spdlog.tar.gz" -C "$BUILD"
    [ -d "$SPDLOG_DIR/include" ] || die "spdlog 解压失败"
}

build_bpf() {
    command -v clang >/dev/null || die "缺少 clang（需要 -target bpf 支持）"

    # BPF 程序只用 UAPI 头（linux/bpf.h、asm/types.h …）。
    # 交叉编译时要指到 aarch64 的 sysroot，否则会去翻宿主机的 x86 头。
    local bpf_incs=()
    if [ "$MODE" = cross ] && [ -d "$SYSROOT/usr/include" ]; then
        bpf_incs+=("-I$SYSROOT/usr/include" "-I$SYSROOT/usr/include/aarch64-linux-gnu")
    else
        local arch_inc="/usr/include/$(uname -m)-linux-gnu"
        [ -d "$arch_inc" ] || arch_inc="/usr/include"
        bpf_incs+=("-I$arch_inc")
    fi

    say "编译 BPF 程序"
    clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 \
          "${bpf_incs[@]}" \
          -c "$ROOT/src/rkperf.bpf.c" -o "$BUILD/rkperf.bpf.o"

    # 交叉编译必须用交叉 ld 打 blob：宿主机 ld 产出的是 x86-64 目标文件，
    # 链 aarch64 二进制时会报 "skipping incompatible bpf_blob.o"。
    local ld_tool="ld"
    if [ "$MODE" = cross ]; then
        ld_tool="${CROSS_PREFIX}ld"
        command -v "$ld_tool" >/dev/null || die "找不到 $ld_tool（交叉链接需要）"
    fi
    # 必须在 build/ 里用相对文件名，否则 ld 会把路径编进符号名
    ( cd "$BUILD" && "$ld_tool" -r -b binary rkperf.bpf.o -o bpf_blob.o )
}

link_native() {
    command -v g++ >/dev/null || die "缺少 g++"
    say "本机编译（aarch64）"
    g++ -O2 -std=c++17 \
        -I"$SPDLOG_DIR/include" -I"$ROOT/src" \
        "$ROOT/src/rkperf.cpp" "$ROOT/src/rktrace.cpp" "$ROOT/src/rksample.cpp" \
        "$ROOT/src/rklog.cpp" "$ROOT/src/rkoplog.cpp" "$ROOT/src/rksummary.cpp" \
        "$BUILD/bpf_blob.o" \
        -lbpf -lelf -lz -lpthread \
        -o "$BUILD/rkperf"
}

link_cross() {
    local gxx="${CROSS_PREFIX}g++"
    command -v "$gxx" >/dev/null || die "找不到 $gxx（设置 CROSS_PREFIX 或安装交叉工具链）"
    [ -d "$SYSROOT/usr/include" ] || die "sysroot 不存在: $SYSROOT（先在板子上跑 --make-sysroot）"

    local extra=()
    local deps_flags=()
    if [ -n "$DEPS" ] && [ -d "$DEPS/include" ]; then
        deps_flags+=("-I$DEPS/include")
        [ -d "$DEPS/lib" ] && deps_flags+=("-L$DEPS/lib")
    fi
    local gccdir
    gccdir=$(ls -d "$SYSROOT"/usr/lib/gcc/aarch64-linux-gnu/* 2>/dev/null | head -1 || true)
    [ -n "$gccdir" ] && extra+=("-L$gccdir")

    say "交叉编译（x86_64 -> aarch64），sysroot=$SYSROOT"
    "$gxx" --sysroot="$SYSROOT" \
        -B"$SYSROOT/usr/lib/aarch64-linux-gnu" \
        -static -O2 -std=c++17 \
        -I"$SPDLOG_DIR/include" -I"$ROOT/src" \
        "${deps_flags[@]}" \
        -I"$SYSROOT/usr/include" -I"$SYSROOT/usr/include/aarch64-linux-gnu" \
        "${extra[@]}" \
        "$ROOT/src/rkperf.cpp" "$ROOT/src/rktrace.cpp" "$ROOT/src/rksample.cpp" \
        "$ROOT/src/rklog.cpp" "$ROOT/src/rkoplog.cpp" "$ROOT/src/rksummary.cpp" \
        "$BUILD/bpf_blob.o" \
        -L"$SYSROOT/usr/lib/aarch64-linux-gnu" \
        -lbpf -lelf -lz -lpthread -lm \
        -o "$BUILD/rkperf"
}

# ---------------- 参数 ----------------

while [ $# -gt 0 ]; do
    case "$1" in
        --native)      MODE=native; shift ;;
        --cross)       MODE=cross; shift ;;
        --deps)        deps; exit 0 ;;
        --make-sysroot) shift; make_sysroot "${1:-}"; exit 0 ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "未知参数: $1（--help 查看用法）" ;;
    esac
done

HOST_ARCH=$(uname -m)
if [ "$MODE" = auto ]; then
    if [ "$HOST_ARCH" = aarch64 ]; then MODE=native; else MODE=cross; fi
fi

say "主机架构: $HOST_ARCH，编译模式: $MODE"
mkdir -p "$BUILD"

fetch_spdlog
build_bpf

if [ "$MODE" = native ]; then
    link_native
else
    link_cross
fi

say "产物: $BUILD/rkperf"
file "$BUILD/rkperf" 2>/dev/null || true
ls -lh "$BUILD/rkperf"
