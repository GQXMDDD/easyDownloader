# third_party/curl

这个目录**不存放** libcurl 源码，只放说明。原因：

1. libcurl 带自己的许可证（curl license），随项目一起分发容易被误认为
   是项目代码的一部分；
2. 系统包管理器 / vcpkg / CMake 的依赖管理方式比"拷一份源码进来"更可控，
   也便于打安全补丁。

## 获取 libcurl 的方式

### MSYS2 / MinGW-w64（Windows，推荐）

```bash
# schannel 后端：TLS 走系统 SChannel，不需要额外 CA 文件，绿色 exe 首选
pacman -S --needed mingw-w64-x86_64-curl-winssl

# 或者 OpenSSL/GnuTLS 后端（需要随程序带 ca-bundle.crt）
pacman -S --needed mingw-w64-x86_64-curl
```

头文件在 `C:/msys64/mingw64/include/curl/`，库在 `C:/msys64/mingw64/lib/libcurl.a`
与 `libcurl.dll.a`。CMakeLists 用 `pkg-config --static --libs libcurl` 拿全量依赖，
再靠 `-static` 选中 `libcurl.a`。

### Debian / Ubuntu

```bash
sudo apt install libcurl4-openssl-dev pkg-config
```

### macOS

```bash
brew install curl pkg-config
```

### vcpkg（跨平台）

```bash
vcpkg install curl[ssl]:x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
```

## 版本要求

- libcurl ≥ 7.32（需要 `CURLOPT_XFERINFOFUNCTION`）
- 实测通过：8.21.0 / Schannel / 静态链接

## 为什么头文件只出现在一个 .c 里

`<curl/curl.h>` 只允许被 `src/core/http_client.c` 包含。上层看不到 `CURL*`、
`CURLcode`、`curl_slist` 这些类型，只看到 `edwl_http_*` 和自定义的
`struct edwl_http_sink`。将来换 WinHTTP 或 nghttp2 时，改这一个文件即可。
