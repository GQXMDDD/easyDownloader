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
- **GitHub 仓库短地址**：`owner/repo.git`、`owner/repo` 或完整仓库地址自动下载默认分支 ZIP
- **跨平台**：平台差异全部隔离在 `platform/{posix,win32}/`，核心层零系统头

---

## 依赖

| 依赖 | 说明 |
|---|---|
| C 编译器 | gcc / clang，C11 |
| libcurl | 开发包（头文件 + 库），HTTPS 靠它 |
| inih | 已随仓库放在 `third_party/inih`（BSD-3），用于解析 `edwl.ini` |
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

```text
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

### 常用命令速查

```bash
# 断点续传：中断后重跑同一条命令即可（进度记在 <文件名>.edwl）
edwl -O big.iso https://example.com/big.iso

# 下载 GitHub 最新正式 Release：自动按平台挑资产 + 自动校验 SHA-256
edwl --release GQXMDDD/easyDownloader -O edwl.exe

# 指定资产名（比如取别的平台的文件），仍然自动取 digest 校验
edwl --release --asset tangram-linux-x64.AppImage \
     -O tangram.AppImage 3477856804/tangram-agent-releases

# 只有直链、拿不到 release 元数据时，自己给哈希
edwl --sha256 89efe2f6e1da6e9205c836955a105d23f8de2e62e3090326efa7dad5ac11e60f \
     -O tangram.AppImage \
     https://github.com/3477856804/tangram-agent-releases/releases/latest/download/tangram-linux-x64.AppImage

# 配置：生成 / 查看 / 改（改了立刻生效，下次运行读取）
edwl --config                    # 生成 edwl.ini；已存在不会覆盖
edwl --config ls                 # 打印当前生效配置
edwl -set Threads=32             # 改并发
edwl -set Blacklist.Enable=off   # Enable 在三个段都有，必须带段名
edwl -set Logging.Level=high     # off / low / high / max
```

校验失败会以退出码 `3` 结束；续传、限速、镜像与校验可以自由组合，例如：

```bash
edwl -j 16 --max-speed 8M --sha256 <64位十六进制> -O big.bin \
     https://a/big.bin https://b/big.bin
```

### GitHub 下载

```powershell
# 自动保存为 easyDownloader.zip，下载默认分支，不假设分支叫 main/master
edwl GQXMDDD/easyDownloader.git
edwl https://github.com/GQXMDDD/easyDownloader.git

# 仍可指定输出名、代理；不会修改系统代理设置
edwl --proxy http://127.0.0.1:7890 -O source.zip GQXMDDD/easyDownloader
```

仓库地址会转换为 GitHub 官方 `codeload.github.com/<owner>/<repo>/zip/HEAD`，
下载源码快照，不执行 `git clone`，也不包含 Git 历史。普通文件、Release 资产、
raw 文件和指定分支/标签的 archive 地址保持原样。

探测只读取最终响应头，不再把不支持 Range 的整个文件下载后丢弃；
已探测到的最终地址用于后续分片，减少 GitHub → CDN 的重复跳转和握手。
如果 CDN 签名链接过期或连接失败，会回到原始地址重新请求，错误响应不会写进文件。
接收缓冲区使用 256 KiB，worker 继续复用各自的连接。

源码 ZIP 通常不支持 Range，因此仍使用单连接；不会虚报多线程加速。
实际速度受网络、GitHub/CDN 限流和代理影响，增加线程不能解决所有线路问题。
镜像模板默认注释，不会自动访问第三方服务；需要时只配置自己信任的真实镜像，
源码包的镜像映射应使用 `codeload.github.com` 作为原始域名。

### 最新 Release 与自动校验

```powershell
# 下载最新正式 Release，自动匹配当前平台/架构的资产并校验 SHA-256
edwl --release -O edwl-latest.exe GQXMDDD/easyDownloader.git

# 多个候选无法唯一匹配时，用精确文件名选择（--asset 隐含 --release）
edwl --asset edwl.exe -O edwl-latest.exe GQXMDDD/easyDownloader
```

只查询 GitHub 标记的 latest 正式 Release，不选择草稿或预发布，不会自动运行下载的文件。
下载本工具自身的 Release 时应指定不同的输出名，避免覆盖正在运行的程序。
自动匹配优先带当前操作系统、架构标记的资产，跳过源码、签名和校验清单；
命名不标准或存在同分候选时显式报错，请使用 `--asset NAME`。

哈希查找顺序：

1. 显式 `--sha256 HEX`（命令行优先）；
2. GitHub API 的资产 `digest`（`sha256:...`）；
3. 同名 `.sha256` / `.sha256sum` / `.sha256.txt` 文件；
4. Release 内的 `SHA256SUMS` / `checksums` 清单；
5. Release 说明里的精确文件名 + SHA-256 条目。

清单支持 GNU `hash  filename`、BSD `SHA256 (filename) = hash`，以及
`filename  SHA-256: hash`，也支持 Release 说明中的文件名/哈希 Markdown 表格。
只匹配所选资产名，避免误用其它文件的哈希；
相互冲突或格式非法的匹配条目会报错。找不到哈希时停止下载并提示使用 `--sha256`，
不会静默跳过校验。
哈希不匹配沿用退出码 `3`；哈希验证完整性，不替代发布者身份/签名验证。

API 和校验文本只通过 HTTPS 获取，复用命令行代理和黑名单，响应限制为 4 MiB；
当前模式面向公开仓库，API 限流或没有正式 Release 时会显式报错。

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
| `--release` / `--latest-release` | 下载 GitHub 最新正式 Release，自动选择资产和 SHA-256 校验 |
| `--asset NAME` | 精确选择 Release 资产名，隐含 `--release` |
| `--no-range` | 禁用分片，强制单连接 |
| `--proxy URL` / `--no-proxy` | 指定代理 / 忽略环境变量代理 |
| `--config` | 生成默认配置文件（已存在则不动，避免冲掉你的修改） |
| `--config ls` | 展示当前生效配置 |
| `--config FILE` | 使用指定配置文件（默认 `./edwl.ini`） |
| `-set K=V` | 修改配置文件（不存在会自动生成），可重复；**同名键要写段名**，如 `Blacklist.Enable=off` |
| `--no-config` | 不读配置文件 |
| `--generate-config` | 生成默认配置模板后退出 |
| `-W, --wait` | 结束前等回车（双击运行时方便看清结果） |
| `--no-blacklist` | 本次忽略黑名单 |
| `-q, --quiet` / `--no-progress` | 安静模式 / 不画进度 |
| `-h` / `-V` | 帮助 / 版本 |

退出码：`0` 成功、`1` 出错、`2` 用法错误、`3` 校验不匹配、`130` 被中断。

进度条属于界面，不受 `Logging.Level` 影响：只要没加 `-q` / `--no-progress` 就会画。

每次下载结束都会打印用时明细（小于 1 秒用毫秒显示，免得"没看清就下完了"）：

```
[result] 大小: 2.0 MiB
[result] 用时: 总计 209 ms（探测 3 ms + 传输 206 ms）   平均 9.7 MiB/s
[result] 分片: 8 片, 4 路并发
[result] 单片用时: 最快 2.5 ms, 最慢 3.8 ms, 平均 3.2 ms
```

`用时` 拆成"探测（拿总长/Range/ETag）+ 传输（真正拉数据）"，平均速度按传输耗时算，
单片用时给出最快/最慢/平均，方便判断是服务器慢还是并行度不够。

---

## 配置文件（edwl.ini）

默认读取当前目录的 `edwl.ini`；`--config FILE` 可指定别的路径，`--no-config` 完全忽略。
**优先级：命令行参数 > 配置文件 > 内置默认值。**

```bash
edwl --generate-config      # 导出一份带注释的模板
```

| 段 | 键 | 说明 |
|---|---|---|
| `[General]` | Threads / DynamicThreads / MaxSpeed / MinSpeed / Timeout / Retries / RetryBackoff / OutputDir / ChunkSizeMB / UserAgent / ResumeFileExt | 并发、限速、超时、重试、输出目录 |
| `[Logging]` | Enable / Level（`off`·`low`·`high`·`max`）/ Output（`console`·`file`·`both`）/ FilePath / Retention（`1000L` 按行、`30T` 按天）/ TimeFormat | 日志 |
| `[Blacklist]` | Enable / Hosts（支持 `*`，`_.example.com` 等价 `*.example.com`）/ IPs（CIDR） | 命中即拒绝 |
| `[Mirrors]` | `原始域名 = 镜像1, 镜像2` | 命中原始域名时自动追加镜像源 |
| `[OpenSpeedy]` | Enable / SpeedMultiplier | 倍率加速：`auto` 或 0.25–8 的固定倍率；`Enable=off` 时整段不生效 |

要点：

- **黑名单在发起任何请求之前生效**，被拦下的目标一个字节都不会被请求；HTTP 跳转后的
  实际地址会再检查一次。命中时退出码 `1`，可用 `--no-blacklist` 临时绕过。
- **日志**：`Level=low` 只出错误与结果，`high` 再加分片进度，`max` 加 libcurl 调试输出。
  按天模式会写成 `edwl-YYYY-MM-DD.log`，并清理超过保留天数的旧文件；按行模式在启动时
  把文件裁到最后 N 行。
- 日志里的时间戳只加在**文件**里（终端输出保持干净），格式由 `TimeFormat` 决定。
- **倍率加速**（`[OpenSpeedy]`）：思路参考 OpenSpeedy 的"倍率"概念，实现是本项目自己的。
  - 固定倍率 `2`：并发 = `Threads × 2`（上限 64），分片同时按倍率切小，让更多连接有活干；
    若同时开 `DynamicThreads`，仍会在 `1…该上限` 之间按速率微调；
  - `auto`：从 `Threads` 起跑，按实测吞吐自动增减并发，最多涨到 3 倍。
  - 倍率过大容易触发服务器限流，掉了会由自适应逻辑退回来。

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
  edwl.h                 公共 API（edwl_main / edwl_version）
  core/                   downloader / segment / task_queue / thread_pool /
                          http_client / source / checksum / github_release
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

- **连接复用**：每个 worker 线程持有一个 libcurl 句柄并复用（keep-alive + DNS 缓存），
  片与片之间不再重新握手；worker 退出时统一释放。
- **限速是近似全局**：libcurl 只有 per-connection 限速，程序按 `max_speed / threads`
  均分到各连接。
- **单流模式不续传**：服务器不支持 Range 时本来就无法续传；此时也不会写 `.edwl`。
- **Windows 路径用 ANSI 接口**：`CreateFileA`，非当前代码页的字符可能打不开。
- **不做 HTTP 认证 / cookie / FTP**：只覆盖 http/https 的匿名下载场景。

---

## 许可

本项目代码是 **MIT**，版权人 `GQXMDDD`，见 `LICENSE`。

**本仓库只放源码，不含预编译二进制**；第三方源码只有 `third_party/inih`（BSD-3，
解析 `edwl.ini` 用）。

⚠️ 但如果你要**分发自己构建的静态 exe**，请先读 `THIRD_PARTY_NOTICES.md`：
MSYS2 现成的 libcurl 静态链接会把 **LGPL-3 的 libidn2 / libunistring** 和
**LGPL-2.1 的 libiconv** 一起链进来。

**解决办法已经准备好并实测通过**：用 `third_party/curl/build-clean-curl.ps1`
自建一份关掉 IDN/PSL/iconv 的 libcurl（见上面「许可干净的二进制」），
产物里就只剩 MIT 的 curl 和 Zlib 的 zlib，附两份许可证即可分发。
构建配置阶段 CMake 也会检查并提醒。
