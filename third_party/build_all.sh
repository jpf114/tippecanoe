#!/usr/bin/env bash
# third_party/build_all.sh —— 离线构建 vendor 三方库（源码随仓库携带，全程不需要网络）
#
# 用途: 为 tippecanoe-db 构建最小化静态库集合:
#   1. PostgreSQL 18.0     -> libpq.a + libpgport.a + libpgcommon_shlib.a（无 SSL/ICU/压缩依赖）
#   2. mongo-c-driver 1.29.0 -> libmongoc-static.a + libbson-static.a（SSL/KMS/SASL/压缩全禁，零外部依赖）
#   3. mongo-cxx-driver r4.0.0 -> libmongocxx-static.a + libbsoncxx-static.a（SSL=OFF，C++17）
#      版本配对：r4.0.0 官方要求 c-driver 1.29.0（ABI 1.0）；4.3+ 要求 GCC 8.1+，
#      r4.0.0 支持 GCC 7（aarch64 离线机 gcc 7.3）
#
# 用法:  ./build_all.sh [安装目录]      （默认 <本目录>/_install）
#        JOBS=8 ./build_all.sh          （并行度，默认 nproc）
#
# 环境要求: cmake >= 3.16、C 编译器、C++17 编译器。无需 OpenSSL/zlib 等任何开发库。
# 注意: mongo-cxx-driver 配置阶段必须能 find_package 到第 2 步安装的 c-driver
#       （CMAKE_PREFIX_PATH 指向），否则它会尝试联网 FetchContent 下载并失败——
#       离线环境下如报 "Required MongoDB C Driver libraries not found"，请检查第 2 步。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${1:-$ROOT/_install}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
ORIG_PWD="$PWD"

PG_SRC="$ROOT/postgresql-18.0"
MONGOC_SRC="$ROOT/mongo-c-driver-1.29.0"
MONGOCXX_SRC="$ROOT/mongo-cxx-driver-r4.0.0"

for d in "$PG_SRC" "$MONGOC_SRC" "$MONGOCXX_SRC"; do
    [ -d "$d" ] || { echo "错误: 缺少源码目录 $d"; exit 1; }
done

mkdir -p "$PREFIX"

echo "==> [1/3] PostgreSQL libpq (static) -> $PREFIX/pg"
(
    cd "$PG_SRC"
    if [ ! -f config.status ]; then
        ./configure --prefix="$PREFIX/pg" \
            --without-readline --without-zlib --without-openssl --without-icu \
            --without-lz4 --without-libxml --without-libxslt --without-gssapi \
            --disable-rpath
    fi
    # NO_GENERATED_HEADERS: 纯前端(libpq)构建，跳过 src/backend 生成头（裁剪后目录不存在）
    make -C src/interfaces/libpq NO_GENERATED_HEADERS=1 -j"$JOBS"
    make -C src/interfaces/libpq NO_GENERATED_HEADERS=1 install
    make -C src/port NO_GENERATED_HEADERS=1 install
    make -C src/common NO_GENERATED_HEADERS=1 install
    # PG18 的 libpq install 不含 postgres_ext.h（属 src/include 全量 install，
    # 会牵入 nodetags.h 等 backend 生成物）；libpq-fe.h 只需这一个头，直接补拷。
    cp src/include/postgres_ext.h "$PREFIX/pg/include/"
)

echo "==> [2/3] mongo-c-driver 1.29.0 (static, SSL/KMS/压缩全禁) -> $PREFIX/mongoc"
# 树外构建（third_party/build/），源码树保持 tarball 原样——其自带 build/cmake/
# 内含 ParseVersion.cmake 等必需 cmake 模块，不可覆盖或删除。
MONGOC_BUILD="$ROOT/build/mongoc"
cmake -S "$MONGOC_SRC" -B "$MONGOC_BUILD" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_SSL=OFF -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_DOCS=OFF \
    -DENABLE_ZLIB=OFF -DENABLE_SNAPPY=OFF -DENABLE_ZSTD=OFF -DENABLE_SASL=OFF \
    -DENABLE_ICU=OFF -DENABLE_CLIENT_SIDE_ENCRYPTION=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX/mongoc" \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$MONGOC_BUILD" -j"$JOBS"
cmake --install "$MONGOC_BUILD"

echo "==> [3/3] mongo-cxx-driver r4.0.0 (static, SSL=OFF, C++17) -> $PREFIX/mongocxx"
MONGOCXX_BUILD="$ROOT/build/mongocxx"
cmake -S "$MONGOCXX_SRC" -B "$MONGOCXX_BUILD" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_PREFIX_PATH="$PREFIX/mongoc/lib/cmake" \
    -DMONGOCXX_ENABLE_SSL=OFF \
    -DBSONCXX_POLY_USE_STD=ON -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX/mongocxx" \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$MONGOCXX_BUILD" -j"$JOBS"
cmake --install "$MONGOCXX_BUILD"
# mongocxx uninstaller 的 .d 清单目录可能落到 CWD（root 环境观察到），
# 会被主 Makefile 的 `*.d` 通配误伤，防御性清理：
rm -rf "$ORIG_PWD/uninstall.sh.d"

echo "==> 完成。产物:"
ls -la "$PREFIX"/pg/lib/*.a "$PREFIX"/mongoc/lib/*.a "$PREFIX"/mongocxx/lib/*static*.a
