#!/bin/bash
# 手动下载GCC依赖（使用国内镜像加速）
# 将依赖放到GCC源码目录下，脚本会自动识别跳过下载

GCC_SRC_DIR="/tmp/gcc-15.2.0"
cd ${GCC_SRC_DIR}

echo "=== 使用国内镜像下载GCC依赖 ==="

# 1. 下载GMP (GNU Multiprecision Library)
echo "[1/4] 下载 GMP 6.2.1..."
wget -c https://mirrors.cloud.tencent.com/gnu/gmp/gmp-6.2.1.tar.bz2

# 2. 下载MPFR (Multiple Precision Floating-Point Reliable)
echo "[2/4] 下载 MPFR 4.1.0..."
wget -c https://mirrors.cloud.tencent.com/gnu/mpfr/mpfr-4.1.0.tar.bz2

# 3. 下载MPC (Multiple Precision Complex)
echo "[3/4] 下载 MPC 1.2.1..."
wget -c https://mirrors.cloud.tencent.com/gnu/mpc/mpc-1.2.1.tar.gz

# 4. 下载ISL (Integer Set Library)
echo "[4/4] 下载 ISL 0.24..."
wget -c https://libisl.sourceforge.io/isl-0.24.tar.bz2 || \
wget -c https://gcc.gnu.org/pub/gcc/infrastructure/isl-0.24.tar.bz2

echo ""
echo "=== 下载完成 ==="
echo "文件列表："
ls -lh gmp-*.tar.bz2 mpfr-*.tar.bz2 mpc-*.tar.gz isl-*.tar.bz2 2>/dev/null

echo ""
echo "现在可以重新运行安装脚本："
echo "  cd /tmp/gcc-15.2.0 && ./contrib/download_prerequisites"
echo "（脚本会检测到文件已存在，跳过下载）"
