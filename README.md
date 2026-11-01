# edwl — 多线程分片下载器

`edwl`是一个用 **C 语言**实现的命令行下载器：
用 HTTP Range 把文件切成若干片，多个连接并行拉取，同时保持 `wget` 式的命令行习惯。

设计红线：**纯用户态、零系统改动**——不写注册表、不设系统代理、不装服务/驱动、
不改 hosts、不碰全局证书库。程序就是一个可执行文件，删掉目录即卸载。

---

## 特性

- **多线程分片下载**：基于 `Range: bytes=a-b`，每片独立连接，`pwrite` 定位写
- **线程池 + 任务队列**：固定 worker 数，避免频繁建线程
- **多源（镜像）切换**：失败进入冷却，健康度指数退避，自动换源
- **整体 SHA-256 校验**：分片可来自不同源，拼完后整文件算一次哈希
- **断点续传**：`.edwl` 边车文件记录每片进度，`Ctrl-C` / 崩溃后可续
- **限速与超时**：走 libcurl 的传输选项（`MAX_RECV_SPEED`、低速判定）
- **单流回退**：服务器不认 Range 时自动退回单连接顺序下载
- **跨平台**：平台差异全部隔离在 `platform/{posix,win32}/`，核心层零系统头

---

## 依赖

| 依赖 | 说明 |
|---|---|
| C 编译器 | gcc / clang，C11 |
| libcurl | 开发包（头文件 + 库），HTTPS 靠它 |
| CMake ≥ 3.16 | 构建 |
| pkg-config | 可选但推荐：用来拿 `libcurl` 的完整链接参数 |

Windows 上推荐 **schannel 后端的 libcurl**（`mingw-w64-x86_64-curl-winssl`）：
TLS 走系统 SChannel，不需要额外 CA bundle 文件，配合静态链接就是单文件绿色 exe。

---

## 构建

以下命令都在本仓库根目录下执行。

### Windows（MSYS2 / MinGW-w64）

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
        mingw-w64-x86_64-ninja mingw-w64-x86_64-pkg-config \
        mingw-w64-x86_64-curl-winssl

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

默认 `-DEDWL_STATIC_CURL=ON`：静态链 libcurl 与 gcc 运行时，产物只依赖系统 DLL
（`KERNEL32/WS2_32/CRYPT32/...`），可以直接拷到别的机器上跑。

### Linux / macOS

```bash
sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev   # Debian/Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 许可干净的二进制（要发 Release 时用）

MSYS2 现成的 `libcurl` 静态链接会把 LGPL 的 libidn2 / libunistring / libiconv
一起链进来。要发布可执行文件，先用脚本自建一份关掉 IDN 的 curl：

```powershell
# 1) 自建干净 libcurl（首次需下载 curl 源码，约 4 MB；编译约 30 秒）
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\third_party\curl\build-clean-curl.ps1 `
  -Prefix "$env:TEMP\edwl_clean_curl"

# 2) 用它构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
      -DEDWL_CURL_PREFIX="$env:TEMP\edwl_clean_curl"
cmake --build build
```

这样出来的 exe 只有 **curl(MIT) + zlib(Zlib)**，体积从 4.39 MB 降到 **1.63 MB**，
分发时附上这两份许可证即可。详见 `THIRD_PARTY_NOTICES.md`。

---

## 用法

```
edwl [选项] URL [镜像URL ...]
```

```bash
# 8 路并发（默认就是 8）
edwl -O ubuntu.iso https://example.com/ubuntu.iso

# 两个镜像源 + 整体校验
edwl -j 16 --sha256 <64位十六进制> -O big.bin https://a/big.bin https://b/big.bin

# 限速 2 MiB/s、只重试 5 次
edwl --max-speed 2M -t 5 -O x.zip https://example.com/x.zip

# 服务器不认 Range 也能下（自动退化为单连接）
edwl --no-range -O y.bin https://example.com/y.bin
```

| 选项 | 说明 |
|---|---|
| `-O, --output FILE` | 输出文件名；以 `/` 结尾时视为目录 |
| `-j, --threads N` | 并发分片数（默认 8） |
| `--min-split-size SIZE` | 单片下限，默认 `1M`；`K/M/G` 或 `KiB/MiB/GiB` |
| `--max-speed RATE` | 全局限速（按连接均分），`0` 不限 |
| `-t, --tries N` | 单片重试次数（默认 3） |
| `-T, --timeout SEC` | 低速判定时长（默认 30s） |
| `--connect-timeout SEC` | 连接超时（默认 15s） |
| `-c, --continue` | 断点续传（默认开启，仅为兼容 wget） |
| `--no-resume` | 忽略断点，从头下 |
| `--sha256 HEX` | 下载完成后校验整文件 SHA-256 |
| `--no-range` | 禁用分片，强制单连接 |
| `--proxy URL` / `--no-proxy` | 指定代理 / 忽略环境变量代理 |
| `-q, --quiet` / `--no-progress` | 安静模式 / 不画进度 |
| `-h` / `-V` | 帮助 / 版本 |

退出码：`0` 成功、`1` 出错、`2` 用法错误、`3` 校验不匹配、`130` 被中断。

---

## 中文显示与编码

程序按"输出到哪里"自动选编码，不需要你改系统设置：

- **输出到控制台**：Windows 上把 UTF-8 转 UTF-16，用 `WriteConsoleW` 写。
  不依赖控制台代码页（`chcp 936` 下也正常显示），也**不会**去改你的代码页；
  POSIX 上直接写 UTF-8（终端本来就是 UTF-8）。
- **重定向到文件 / 管道**：原样输出 UTF-8 字节，方便脚本和下游工具处理。

唯一需要注意的是 PowerShell 5.1 **捕获**输出时（`edwl --help | ...`、`$x = edwl ...`），
它会按自己的 `[Console]::OutputEncoding`（中文系统默认 GBK）去解码，可能仍显示乱码。
先执行一句即可：

```powershell
[Console]::OutputEncoding = [Text.Encoding]::UTF8
```

直接运行（不捕获）时不受此影响。

---

## 断点文件（`.edwl`）

下载并行分片时，输出文件旁边会有 `<输出名>.edwl`：

```
edwl-meta 1
size 3145728
etag "3145728"
mtime 
pieces 4
piece 0 786431 589824        # begin end next_pos
```

- `next_pos` 是"这一片下一个要写的偏移"，所以续传时从 `next_pos` 继续请求
- 续传前会校验总长、`ETag` / `Last-Modified`：服务器上的东西变了就作废重下
- 下载成功后自动删除；`--no-resume` 或 `Ctrl-C` 后按提示重新运行即可续传

---

## 目录结构

```
include/edwl/
  wgetx.h                 公共 API（wgetx_main / wgetx_version）
  core/                   downloader / segment / task_queue / thread_pool /
                          http_client / source / checksum
  platform/               platform.h 统一入口 + file_io/thread/time/console
  util/                   log / buffer / url
src/
  main.c                  命令行解析
  core/                   核心实现
  platform/posix|win32/   两套平台实现（+ 共享的进度格式化）
  util/                   工具实现
tests/                    5 个单元测试（URL / 分片 / 队列 / SHA-256 / 文件 IO）
docs/                     架构说明、移植指南
```

依赖规则（最重要的一条）：`core/` 与 `util/` 下的 `.c` 只允许包含标准 C 头、
项目自己的头、以及 `<edwl/platform/platform.h>`。`<curl/curl.h>` **只**出现在
`src/core/http_client.c`；否则换 HTTP 库时要动一片文件。

---

## 已知限制

- **每片一条连接**：分片之间不复用 TCP/TLS 连接（有意为之，便于故障隔离）。
  小文件分片少时，握手开销占比会偏高。
- **限速是近似全局**：libcurl 只有 per-connection 限速，程序按 `max_speed / threads`
  均分到各连接。
- **单流模式不续传**：服务器不支持 Range 时本来就无法续传；此时也不会写 `.edwl`。
- **Windows 路径用 ANSI 接口**：`CreateFileA`，非当前代码页的字符可能打不开。
- **不做 HTTP 认证 / cookie / FTP**：只覆盖 http/https 的匿名下载场景。

---

## 许可

本项目代码是 **MIT**，版权人 `GQXMDDD`，见 `LICENSE`。

**本仓库只放源码，不含任何第三方代码，也不含预编译二进制**——只发布源码时没有
任何第三方许可义务。唯一的第三方依赖是 libcurl（MIT）。

⚠️ 但如果你要**分发自己构建的静态 exe**，请先读 `THIRD_PARTY_NOTICES.md`：
MSYS2 现成的 libcurl 静态链接会把 **LGPL-3 的 libidn2 / libunistring** 和
**LGPL-2.1 的 libiconv** 一起链进来。

**解决办法已经准备好并实测通过**：用 `third_party/curl/build-clean-curl.ps1`
自建一份关掉 IDN/PSL/iconv 的 libcurl（见上面「许可干净的二进制」），
产物里就只剩 MIT 的 curl 和 Zlib 的 zlib，附两份许可证即可分发。
构建配置阶段 CMake 也会检查并提醒。
