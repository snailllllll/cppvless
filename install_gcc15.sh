#!/bin/bash
# GCC 15.2 安装脚本
# 预计时间：30分钟-2小时
# 需要磁盘空间：约10GB

set -e  # 遇到错误立即退出

INSTALL_PREFIX="/opt/gcc-15.2"
GCC_VERSION="15.2.0"
NPROC=8  # 用户指定8核并行编译

echo "=== GCC ${GCC_VERSION} 安装脚本 ==="
echo "安装目录: ${INSTALL_PREFIX}"
echo "并行编译线程数: ${NPROC}"
echo "预计时间: 30分钟-2小时"
echo ""

# 1. 安装编译依赖
echo "[1/5] 安装编译依赖..."
sudo yum install -y \
    wget \
    bzip2 \
    gcc \
    gcc-c++ \
    make \
    gmp-devel \
    mpfr-devel \
    libmpc-devel \
    isl-devel \
    zlib-devel

# 2. 下载GCC源码
echo "[2/5] 下载GCC ${GCC_VERSION} 源码..."
cd /tmp
if [ ! -f gcc-${GCC_VERSION}.tar.xz ]; then
    wget https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz
fi

# 3. 解压源码
echo "[3/5] 解压源码..."
if [ ! -d gcc-${GCC_VERSION} ]; then
    tar xf gcc-${GCC_VERSION}.tar.xz
fi

# 4. 下载前置依赖（GMP, MPFR, MPC, ISL）
echo "[4/5] 下载GCC依赖..."
cd gcc-${GCC_VERSION}
./contrib/download_prerequisites

# 5. 编译安装
echo "[5/5] 编译并安装GCC（这可能需要30分钟-2小时）..."
mkdir -p build
cd build

../configure \
    --prefix=${INSTALL_PREFIX} \
    --enable-languages=c,c++ \
    --disable-multilib \
    --enable-bootstrap \
    --enable-lto \
    --enable-threads=posix \
    --enable-checking=release \
    --enable-libstdcxx-backtrace \
    --enable-vtable-verify \
    --with-system-zlib

echo "开始编译...（使用${NPROC}个线程）"
make -j${NPROC}

echo "安装到 ${INSTALL_PREFIX}..."
sudo make install

# 6. 配置环境
echo ""
echo "=== 安装完成 ==="
echo "GCC 15.2 已安装到: ${INSTALL_PREFIX}"
echo ""
echo "使用方法："
echo "  临时使用: export PATH=${INSTALL_PREFIX}/bin:\$PATH"
echo "  永久使用: echo 'export PATH=${INSTALL_PREFIX}/bin:\$PATH' >> ~/.bashrc"
echo ""
echo "验证安装: ${INSTALL_PREFIX}/bin/gcc --version"
