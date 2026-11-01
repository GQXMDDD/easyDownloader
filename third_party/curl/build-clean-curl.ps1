# Build a license-clean static libcurl for edwl.
#
# WHY
#   The ready-made MSYS2 package (mingw-w64-x86_64-curl-winssl) is linked
#   against libidn2 / libunistring (LGPL-3.0+) and libiconv (LGPL-2.1+).
#   Static-linking those into edwl.exe means LGPL obligations as soon as you
#   distribute the binary. This script builds curl from source with IDN, PSL,
#   libiconv, brotli, zstd, ssh2 and LDAP disabled, so the resulting static
#   library only pulls in MIT / BSD / Zlib components (plus Windows system DLLs).
#
# USAGE
#   powershell -NoProfile -ExecutionPolicy Bypass -File build-clean-curl.ps1
#
#   Then build edwl against it:
#     cmake -S <edwl>\main -B <builddir> -G Ninja -DCMAKE_BUILD_TYPE=Release `
#           -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
#           -DEDWL_CURL_PREFIX=<printed prefix>
#
# VERIFIED (2026-11-01, curl 8.21.0, mingw-w64 gcc 16.1.0)
#   build time ~26 s, edwl.exe 4.39 MB -> 1.63 MB,
#   nm shows 0 idn2_/u8_/libiconv/psl_/ZSTD_/Brotli/libssh2 symbols,
#   imports are system DLLs only.

param(
    [string]$Version = "8.21.0",
    [string]$Prefix  = "$env:TEMP\edwl_clean_curl",
    [string]$Work    = "$env:TEMP\curl_src",
    [string]$Cc      = "C:/msys64/mingw64/bin/gcc.exe",
    [string]$Cmake   = "C:/msys64/mingw64/bin/cmake.exe",
    [string]$Ninja   = "C:/msys64/mingw64/bin/ninja.exe"
)

$ErrorActionPreference = 'Stop'

function Fail($msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

foreach ($tool in @($Cc, $Cmake, $Ninja)) {
    if (-not (Test-Path -LiteralPath $tool)) { Fail "missing tool: $tool" }
}

$tarball  = Join-Path $Work "curl-$Version.tar.gz"
$srcdir   = Join-Path $Work "curl-$Version"
$builddir = Join-Path $Work "build-clean"

New-Item -ItemType Directory -Force -Path $Work | Out-Null

# 1. source
if (-not (Test-Path -LiteralPath $srcdir)) {
    if (-not (Test-Path -LiteralPath $tarball)) {
        Write-Host "downloading curl $Version ..."
        & "$env:SystemRoot\System32\curl.exe" -L --fail -o $tarball `
            "https://curl.se/download/curl-$Version.tar.gz"
        if ($LASTEXITCODE -ne 0) { Fail "download failed" }
    }
    Write-Host "extracting ..."
    & "$env:SystemRoot\System32\tar.exe" -xzf $tarball -C $Work
    if ($LASTEXITCODE -ne 0) { Fail "extract failed" }
}
if (-not (Test-Path -LiteralPath (Join-Path $srcdir "CMakeLists.txt"))) {
    Fail "source tree not found: $srcdir"
}

# 2. configure: no IDN, no PSL, no libiconv, no brotli/zstd/ssh2/LDAP, Schannel TLS
Write-Host "configuring (no IDN / PSL / libiconv / brotli / zstd / ssh2 / LDAP) ..."
& $Cmake -S $srcdir -B $builddir -G Ninja `
    -DCMAKE_C_COMPILER="$Cc" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_INSTALL_PREFIX="$Prefix" `
    -DBUILD_SHARED_LIBS=OFF `
    -DBUILD_CURL_EXE=OFF `
    -DBUILD_EXAMPLES=OFF `
    -DBUILD_TESTING=OFF `
    -DCURL_USE_SCHANNEL=ON `
    -DUSE_LIBIDN2=OFF `
    -DCURL_USE_LIBPSL=OFF `
    -DCURL_ZLIB=ON `
    -DCURL_BROTLI=OFF `
    -DCURL_ZSTD=OFF `
    -DCURL_USE_LIBSSH2=OFF `
    -DCURL_DISABLE_LDAP=ON `
    -DUSE_NGHTTP2=OFF
if ($LASTEXITCODE -ne 0) { Fail "configure failed" }

# 3. build + install
Write-Host "building ..."
& $Cmake --build $builddir
if ($LASTEXITCODE -ne 0) { Fail "build failed" }

& $Cmake --install $builddir
if ($LASTEXITCODE -ne 0) { Fail "install failed" }

$lib = Join-Path $Prefix "lib\libcurl.a"
if (-not (Test-Path -LiteralPath $lib)) { Fail "libcurl.a not produced" }

Write-Host ""
Write-Host "OK: $lib" -ForegroundColor Green
Write-Host "prefix for edwl: $Prefix"
Write-Host ""
Write-Host "next:"
Write-Host "  cmake -S <edwl>\main -B <builddir> -G Ninja -DCMAKE_BUILD_TYPE=Release ``"
Write-Host "        -DCMAKE_C_COMPILER=$Cc -DEDWL_CURL_PREFIX=$Prefix"
