# 架构说明

## 1. 分层

```
┌───────────────────────────────────────────────────┐
│ main.c / CLI 解析（wget 式参数）                  │
├───────────────────────────────────────────────────┤
│ core/          纯 C，无任何系统头                  │
│   downloader ──► thread_pool ──► task_queue       │
│        │                                          │
│        ├──► http_client（唯一包含 <curl/curl.h>）  │
│        ├──► segment（分片模型，纯数据 + 纯函数）   │
│        ├──► source（多源健康度）                   │
│        └──► checksum（自实现 SHA-256）             │
├───────────────────────────────────────────────────┤
│ platform/platform.h   抽象接口（不透明句柄）       │
├───────────────────────────────────────────────────┤
│ platform/posix/*  ｜  platform/win32/*            │
└───────────────────────────────────────────────────┘
```

**依赖规则**：`core/` 与 `util/` 下的 `.c` 只允许包含标准 C 头、项目自己的头、
以及 `<edwl/platform/platform.h>`。唯一的例外是 `http_client.c`——它是 libcurl
封装层，`<curl/curl.h>` 只出现在它内部；`http_client.h` 对外只暴露自定义结构体
和回调，将来换 WinHTTP / nghttp2 只改一个文件。

平台层用 `intptr_t` 承载句柄（POSIX 下是 `int` fd，Win32 下是 `HANDLE`），
核心层只通过函数操作，不解读其内容。

## 2. 一次下载的时序

```
main
 └─ wgetx_download_run
     ├─ 1. resolve_output        URL → 输出文件名（+ .edwl 边车路径）
     ├─ 2. probe_any             逐个源发 Range: bytes=0-0
     │      └─ 得到 total / range_supported / ETag / Last-Modified
     ├─ 3. 选择模式
     │      ├─ 并行：认 Range 且 total ≥ min_split 且 threads > 1
     │      └─ 单流：其余情况
     ├─ 4. parallel_download
     │      ├─ 切片（或读 .edwl 恢复）
     │      ├─ open + ftruncate(total)     预分配，避免写时扩张竞态
     │      ├─ 先落一次 .edwl
     │      ├─ 起 thread_pool（workers = min(threads, 片数)）
     │      ├─ 起 monitor 线程（200ms 一刷：进度 / 速度 / 定期落断点）
     │      ├─ 所有未完成片入队
     │      ├─ wait_idle → 停 monitor → 关池
     │      └─ 全部完成则删 .edwl，否则保存现场
     ├─ 5. 可选 SHA-256 校验
     └─ 6. 摘要输出
```

## 3. 关键数据结构

### 分片（`core/segment.h`）

```c
struct wgetx_segment {
        uint64_t index;
        uint64_t begin;    /* 闭区间起点 */
        uint64_t end;      /* 闭区间终点 */
        uint64_t pos;      /* 下一个要写的偏移，∈ [begin, end+1] */
        int      state;    /* PENDING / ACTIVE / DONE */
        unsigned attempts;
};
```

`pos` 就是断点：`pos == end + 1` 表示这片完成。因为 `pos` 只增不改写，
任意时刻中断都能安全恢复。

切片策略：目标每路约两片（留一点动态均衡余量），再夹在
`[min_piece_size, max_piece_size]` 之间，最后一片吃掉除不尽的尾巴，
保证 `[0, total-1]` 完整覆盖且互不重叠。

### 断点文件

```
edwl-meta 1
size <总长>
etag <ETag，可能为空>
mtime <Last-Modified，可能为空>
pieces <片数>
piece <begin> <end> <pos>      ← 每片一行
```

写入方式是"写 `.tmp` → 删旧的 → `rename`"，避免断电留下半截元数据。
加载时做四重校验：总长一致、ETag/Last-Modified 一致（两边都非空才比）、
区间连续、每片 `pos` 合法。任何一项不符就整份作废，重新分片。

## 4. 并发与加锁

| 数据 | 保护方式 |
|---|---|
| 各片 `pos` / `state` / `attempts` | `dl_state.mtx`（每次写盘后加锁更新） |
| `single_done` | 同上 |
| 任务队列 | `wgetx_task_queue` 内部自己的 mutex + cond |
| 未完成计数 | `wgetx_thread_pool` 内部 mutex + cond |
| 源健康度 | `wgetx_sources` 内部 mutex |
| 进度快照 | 在 `mtx` 内取一份拷贝，出了锁再画 |

`outstanding` 计数（而不是"队列为空"）才是 `wait_idle()` 的依据：worker 可能在
任务内部再次入队重试任务，只等队列空会提前返回。

## 5. 失败处理

- **单片失败**：`attempts++` → 退避 `300ms × 2^n`（封顶 3s）→ 换下一个源重试，
  最多 `1 + retries` 次。重试用尽则该片回到 `PENDING`，最终收尾时判定失败。
- **源健康度**：失败进入冷却（1s→2s→…→30s），成功立刻清零；`sources_pick()`
  轮转跳过冷却中的源。
- **服务器无视 Range**（本该 206 却回 200）：立即置 `fatal`，停止并报错——
  这种响应下按分片偏移写盘会直接写坏文件。
- **连接提前断开**（`WGETX_HTTP_ERR_SHORT`）：不算致命，`pos` 已前进，
  重试会从断点继续，最终拼出完整文件。
- **本地写失败**：`WGETX_HTTP_ERR_LOCAL_IO`，按普通失败重试一次后放弃。
- **401/403/404**：探测阶段直接换源，不做无意义重试。

## 6. 中断与续传

- 平台层把 `Ctrl-C` 转成一个进程内标志（POSIX `sigaction`，Win32
  `SetConsoleCtrlHandler`），核心层在 libcurl 的 `XFERINFO` 回调里轮询。
- 中断时 worker 立刻退出，`pos` 已经反映了真实落盘进度；收尾把 `ACTIVE` 退回
  `PENDING` 并落一次 `.edwl`，进程以退出码 `130` 结束。
- 下次运行若发现 `.edwl` 匹配，就从每片的 `pos` 继续；输出文件已预分配到位，
  不需要重写。

## 7. 校验

`checksum.c` 自带 SHA-256（FIPS 180-4），不引 OpenSSL。分片可能来自不同源，
所以哈希只能在拼完之后对整文件算一次：流式 `pread` 64 KiB 一块喂进哈希，
`O(1)` 内存。算完与 `--sha256` 比对，不匹配退出码 `3`，文件保留供排查。

## 8. 与原设计稿的差异（都是实现时才发现的）

| 位置 | 原设计写的 | 实际做法 | 原因 |
|---|---|---|---|
| 头文件目录 | 正文同时出现 `include/wgetx/...` 与 `edwl/platform/platform.h` | 统一用 `include/edwl/`，符号前缀仍是 `wgetx_` | 取"依赖规则"里明确写死的 `edwl/platform/platform.h` |
| Win32 `pwrite` | "句柄不必以 `FILE_FLAG_OVERLAPPED` 打开" | **必须**带 `FILE_FLAG_OVERLAPPED` | 同步句柄会忽略 `OVERLAPPED.Offset`、改用内部文件指针，多线程共用必然互相踩；实测 `ERROR_HANDLE_EOF` 也只在重叠句柄上出现 |
| `file_open_write` | `CreateFileA(..., CREATE_ALWAYS, ...)` | `OPEN_ALWAYS`（POSIX 侧 `O_CREAT` 不带 `O_TRUNC`） | `CREATE_ALWAYS` 会把续传所需的已下载内容清掉；长度统一由 `file_truncate` 显式设定 |
| 平台 API | 只列了 5 个文件 IO 函数 | 增加 `file_open_read` / `file_size` / `cond_timedwait` / `thread_current_id` / 中断标志 | 校验阶段需要只读句柄；其它是线程池与 Ctrl-C 的必要补充 |
| 测试 | `test_segment/test_queue/test_url` | 另加 `test_checksum` / `test_file_io` | 哈希与定位读写的边界（EOF）是最容易错的地方，必须有回归 |

## 9. 线程模型

```
main thread                      monitor thread          worker × N
    │                                  │                     │
    ├─ submit(片 1..M) ─────────────────┼─────────────────────┤
    ├─ wait_idle()                     │                     │
    │                                  │  200ms 循环：        │  pop 任务
    │                                  │   取快照→算速度      │  取源→连接
    │                                  │   画进度→存断点      │  拉 Range
    │                                  │                     │  pwrite
    └─ 全部完成 ←──────────────────────┴─────────────────────┘
```

worker 之间不共享 libcurl 句柄：每次传输新建一个 `wgetx_http`，用完销毁。
好处是故障隔离彻底（一个连接炸了不影响别的），代价是每片一次 TCP/TLS 握手。
