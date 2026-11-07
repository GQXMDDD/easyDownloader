# 第三方组件与许可证说明

> 这是工程层面的许可证检查记录，不构成法律意见。真要商用/大规模分发，
> 请让法务确认。

## 结论（先看这段）

1. **本仓库只包含本项目自己写的 C 代码**，许可证 MIT，版权人 `GQXMDDD`（见 `LICENSE`）。
2. **仓库里只有一份第三方源码**：`third_party/inih`（BSD-3，用于解析 `edwl.ini`）；
   **没有预编译二进制**。
3. 依赖只有 libcurl 一个（MIT，宽松），构建时从系统/包管理器取。
4. ⚠️ **MSYS2 现成的 `mingw-w64-x86_64-curl-winssl` 静态链接后会带进 LGPL 组件**
   （libidn2 / libunistring / libiconv）。直接分发那种 exe 会产生 LGPL 义务。
5. ✅ **已经准备好干净方案并且实测通过**：用 `third_party/curl/build-clean-curl.ps1`
   自建一份关掉 IDN/PSL/iconv 的 libcurl，得到的静态 exe 里只剩 **curl(MIT) + zlib(Zlib)**
   ，分发时只需要附这两份许可证原文。

---

## 一、本仓库自身的许可

- 许可证：MIT（`LICENSE`），版权人：`GQXMDDD`。
- 代码来源：全部为本项目新写。
- 参考过的是 **aria2 的架构思路**（`Range: bytes=a-b` 的构造方式、只有 206 才认分片、
  Piece/Segment 分层），**没有复制任何代码**；仓库内不含任何 GPL/LGPL 代码。
  （aria2 是 GPL-2，复制其代码会把本项目传染成 GPL，这里刻意避开了。）

---

## 二、构建期依赖（不在本仓库内）

| 组件 | 许可证 | 说明 |
|---|---|---|
| libcurl | MIT（curl license） | 通过 pkg-config 找；可用自建版本替换 |
| inih | BSD-3-Clause | 随仓库分发，`third_party/inih` |
| 系统 TLS | Windows SChannel | 操作系统自带，不随项目分发 |

---

## 三、静态 exe 里到底链进了什么（实测，`nm` 统计符号）

### 3.1 用 MSYS2 现成包（默认方式，**不建议拿去分发**）

| 组件 | 许可证 | 传染性 | 符号数 |
|---|---|---|---|
| curl | MIT | 否 | `curl_*` |
| libssh2 | BSD-3 | 否 | 291 |
| libpsl | MIT | 否 | 29 |
| zlib | Zlib | 否 | 7 |
| zstd | BSD-3 | 否 | 135 |
| brotli | MIT | 否 | 20 |
| **libidn2** | **LGPL-3.0-or-later** | **是** | 32 |
| **libunistring** | **LGPL-3.0-or-later** | **是** | 72 |
| **libiconv** | **LGPL-2.1-or-later** | **是** | 7 |

产物 4.39 MB，导入 `WLDAP32` 等 10 个系统 DLL。

### 3.2 用本仓库的脚本自建 curl（**推荐，文档已验证**）

`third_party/curl/build-clean-curl.ps1` 用 curl 源码构建：
关掉 `IDN`、`PSL`、`libiconv`、`brotli`、`zstd`、`ssh2`、`LDAP`，TLS 用 Schannel。

| 组件 | 许可证 | 符号数 |
|---|---|---|
| curl | MIT | `curl_easy_init` 等 |
| zlib | Zlib | `inflate` 23、`crc32` 7 |
| libidn2 / libunistring / libiconv / libpsl / zstd / brotli / libssh2 | —— | **全部 0** |

实测结果：

| 指标 | 自建前（系统包） | 自建后 |
|---|---|---|
| `edwl.exe` 大小 | 4.39 MB | **1.63 MB** |
| LGPL 符号 | 有 | **0** |
| 导入 DLL | 10 个系统 DLL（含 WLDAP32） | **8 个系统 DLL** |
| 编译耗时 | —— | 约 26 秒 |
| 功能验证 | —— | 分片 / 单流 / 校验 / 404 / 续传 / HTTPS 重定向 全通过 |

---

## 四、分发路线（按省事程度排序）

### 路线 A：只发源码（当前仓库状态）

- 仓库里只有 MIT 代码，什么都不用附带，没有 LGPL/GPL 义务。
- 使用者自己构建出来的二进制，由使用者自己承担分发责任。

### 路线 B：发"干净静态 exe"（已验证，推荐用于发 Release）

```powershell
# 1) 构建干净的 libcurl（第一次要下源码，约 4 MB；之后走缓存）
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\third_party\curl\build-clean-curl.ps1 `
  -Prefix "$env:TEMP\edwl_clean_curl"

# 2) 用它构建 edwl（-DEDWL_CURL_PREFIX 会自动把它的 libcurl.pc 加进 PKG_CONFIG_PATH）
cmake -S main -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
      -DEDWL_CURL_PREFIX="$env:TEMP\edwl_clean_curl"
cmake --build build
```

构建时可以确认"干净"：配置阶段**不再**出现 LGPL 警告，产物 `nm` 里
`idn2_`/`u8_`/`libiconv`/`psl_` 都是 0。

**分发这份 exe 时需要附带**：

1. 本项目 `LICENSE`（MIT）
2. curl 的 `LICENSE`（**MIT**）
3. zlib 的 `LICENSE`（**Zlib**）
4. inih 的 `third_party/inih/LICENSE.txt`（**BSD-3**）

就这三份，没有 LGPL，不需要"提供可重链接目标文件"那类动作。

### 路线 C：发"exe + 几个 DLL"

动态链接时 LGPL 库以独立 DLL 存在，满足"可替换"要求；但要一起拷 DLL
（`libcurl-4.dll`、`libidn2-0.dll`、`libunistring-5.dll`、`libiconv-2.dll` …）
并附各许可证文本与源码获取方式。比路线 B 麻烦，只在必须用现成包时选它。

---

## 五、许可证原文在哪

MSYS2 里按短名放：`C:\msys64\mingw64\share\licenses\<短名>\`

```
curl\LICENSE          zlib\LICENSE           libssh2\LICENSE
libpsl\COPYING        zstd\LICENSE           brotli\LICENSE
libunistring\LICENSE、LICENSE.LIB             libiconv\COPYING、COPYING.LIB
```

`libidn2` 包没有安装许可证文件，需要时从
<https://www.gnu.org/licenses/lgpl-3.0.html> 或源码包的 `COPYING` 取。

自建 curl 的源码树里也有 `curl-<版本>/COPYING`（MIT），可直接随二进制分发。

---

## 六、构建时的自动提醒

`CMakeLists.txt` 在配置阶段会检查静态链接列表里有没有 `idn2`：

- 有 → 打印 `WARNING`，提示"分发前先读本文件"；
- 没有（路线 B）→ 不打扰你。

---

## 七、一句话总结

- **只发源码** → 零义务。
- **要发 exe** → 先跑 `build-clean-curl.ps1`，再用 `-DEDWL_CURL_PREFIX` 构建；
  那样 exe 里只剩 MIT 的 curl 与 Zlib 的 zlib，附两份许可证即可。
  **不要**直接静态链接 MSYS2 现成的 libcurl 去分发。
