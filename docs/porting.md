# 移植指南（新增一个平台）

核心层与平台层之间只有 `include/edwl/platform/` 这一组头文件是契约。
新增平台 = 把这 5 组函数各实现一遍，核心层一行不改。

## 1. 需要实现的文件

在 `src/platform/<新平台>/` 下建四份实现（文件名随意，CMake 里列出来即可）：

| 头文件 | 必须实现的函数 |
|---|---|
| `file_io.h` | `edwl_file_open_write` / `edwl_file_open_read` / `edwl_file_truncate` / `edwl_file_size` / `edwl_file_pwrite` / `edwl_file_pread` / `edwl_file_close` |
| `thread.h` | `edwl_thread_create` / `edwl_thread_join` / `edwl_thread_current_id` / `edwl_mutex_*` / `edwl_cond_*`（含 `timedwait`） |
| `time.h` | `edwl_now_ms` / `edwl_now_us` / `edwl_sleep_ms` |
| `console.h` | `edwl_console_init` / `shutdown` / `ansi` / `tty` / `width` / `write` / `write_err` / `progress_draw` / `progress_end` / `install_interrupt` / `interrupt_requested` / `interrupt_reset` / `format_size` |

进度行的文本排版逻辑已经抽到 `src/platform/console_fmt.c`（平台无关），
新平台的 `progress_draw` 只需要：取宽度 → 调 `edwl_console_format_progress()`
→ 决定用 `\r` 覆盖还是逐行输出。

## 2. 接入构建

在 `CMakeLists.txt` 的平台分支里加一段：

```cmake
if(WIN32)
        ...
elseif(APPLE)
        list(APPEND EDWL_LIB_SOURCES src/platform/macos/*.c)
else()
        ...
endif()
```

如果新平台的 libcurl 也需要特殊链接参数，走 `pkg-config`（多数系统都有
`libcurl.pc`），不用改 CMake 逻辑。

## 3. 几个必须想清楚的点

### 3.1 定位读写必须是"每次调用独立偏移"

断点续传的核心就是多线程同时写同一个文件的不同位置：

- 有 `pwrite/pread` 的系统：直接用，注意 `off_t` 是否 64 位
  （32 位系统要 `-D_FILE_OFFSET_BITS=64`）。
- 没有 `pwrite` 的系统：**不能**用 `seek + write`，那是进程级共享的文件指针，
  多线程必然写串。要么用平台的重叠/定位 IO（Win32 的 `OVERLAPPED`、
  macOS 的 `pwrite`），要么在平台层内部加锁 + 手动 seek（会牺牲并发）。
- Windows 特别提醒：只有以 `FILE_FLAG_OVERLAPPED` 打开的句柄，`OVERLAPPED.Offset`
  才会被尊重。同步句柄会忽略偏移、改用内部文件指针。本项目的
  `file_io_win32.c` 就是踩过这个坑之后写的，里面有详细注释。

### 3.2 读文件到结尾

- POSIX：`pread` 在 EOF 返回 0。
- Windows（重叠句柄）：`ReadFile` 在 EOF 返回 `FALSE` + `ERROR_HANDLE_EOF`。
  必须把这个错误当成"读到 0 字节"，否则 SHA-256 校验永远读不到结尾
  （`test_file_io` 覆盖了这条）。

### 3.3 条件变量必须和同一把互斥量配对

`edwl_cond_wait(c, m)` 的语义是"原子地释放 m 并等待"，所以调用方必须先持有 m。
Win32 侧是 `SleepConditionVariableCS(&cv, &cs, INFINITE)`，POSIX 侧是
`pthread_cond_wait(&cond, &mutex)`，`timedwait` 的返回值约定是
**1=被唤醒 / 0=超时 / -1=出错**。

### 3.4 时钟必须单调

测速、超时、退避都用 `edwl_now_ms()`。不要用墙钟（`time()` / `GetLocalTime`）：
NTP 校时会跳，速度会算出负数或尖峰。Windows 用 `QueryPerformanceCounter`。

### 3.5 Ctrl-C / 中断

平台层只需要把中断转成一个进程内标志，核心层在 libcurl 的
`CURLOPT_XFERINFOFUNCTION` 回调里轮询它——不要试图从信号处理函数里直接
回调到下载逻辑，那里面能做的操作极其有限。

### 3.6 字符编码

Windows 控制台默认代码页是 GBK（936），而源码里的中文是 UTF-8。
`console_win32.c` 在 `init()` 里临时 `SetConsoleOutputCP(CP_UTF8)`、
`shutdown()` 里还原——只影响当前控制台窗口，不碰注册表。
新平台如果也有类似的编码陷阱，照此处理。

## 4. 移植后怎么验证

1. `ctest --output-on-failure`：五个单元测试全绿（`test_file_io` 会真正
   落盘做定位读写，能第一时间抓住 3.1/3.2 的坑）。
2. 本地起一个支持 Range 的 HTTP 服务，跑：
   ```bash
   edwl -j 4 --min-split-size 256K --sha256 <已知值> -O out.bin <url>
   ```
   校验值一致说明分片拼装正确。
3. 下到一半按 `Ctrl-C`，确认生成 `.edwl`、退出码 130；再跑一次确认输出
   "从断点恢复"，最终哈希一致。
4. 换一个不支持 Range 的地址（例如返回 200 的静态服务），确认自动退化为
   单连接且内容正确。
