cmake_minimum_required(VERSION 3.16)
project(edwl VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(EDWL_BUILD_TESTS "构建单元测试" ON)
option(EDWL_STATIC_CURL "静态链接 libcurl（Windows 下可得到绿色单文件 exe）" ON)

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------

set(EDWL_LIB_SOURCES
	src/core/checksum.c
	src/core/downloader.c
	src/core/http_client.c
	src/core/segment.c
	src/core/source.c
	src/core/task_queue.c
	src/core/thread_pool.c
	src/util/buffer.c
	src/util/log.c
	src/util/url.c
	src/platform/console_fmt.c
)

if(WIN32)
	list(APPEND EDWL_LIB_SOURCES
		src/platform/win32/console_win32.c
		src/platform/win32/file_io_win32.c
		src/platform/win32/thread_win32.c
		src/platform/win32/time_win32.c
	)
else()
	list(APPEND EDWL_LIB_SOURCES
		src/platform/posix/console_posix.c
		src/platform/posix/file_io_posix.c
		src/platform/posix/thread_posix.c
		src/platform/posix/time_posix.c
	)
endif()

# ---------------------------------------------------------------------------
# libcurl
#
# 直接问 pkg-config 要 --cflags / --libs：
#   - 静态链接在 Windows 上必须拿到完整依赖列表（zlib/zstd/brotli/libssh2 …），
#     find_package(CURL) 只给一个 -lcurl，配 -static 会链接失败。
#   - CMake 自带的 pkg_check_modules 会把 -lcurl 解析成 libcurl.dll.a 的绝对
#     路径，导致静态链接被悄悄降级成动态链接，所以这里不用它。
# ---------------------------------------------------------------------------

find_package(PkgConfig QUIET)
set(EDWL_CURL_FOUND FALSE)
set(EDWL_CURL_BACKEND "none")

# 可选：指向自建的 libcurl 前缀（见 third_party/curl/build-clean-curl.ps1），
# 该前缀里有 lib/pkgconfig/libcurl.pc，优先级高于系统包。
set(EDWL_CURL_PREFIX "" CACHE PATH "自建 libcurl 的前缀（含 include/ 与 lib/pkgconfig）")
if(EDWL_CURL_PREFIX)
	file(TO_CMAKE_PATH "${EDWL_CURL_PREFIX}" _edwl_curl_prefix)
	if(EXISTS "${_edwl_curl_prefix}/lib/pkgconfig/libcurl.pc")
		set(ENV{PKG_CONFIG_PATH}
		    "${_edwl_curl_prefix}/lib/pkgconfig;$ENV{PKG_CONFIG_PATH}")
	else()
		message(WARNING "EDWL_CURL_PREFIX 下没有 lib/pkgconfig/libcurl.pc：${_edwl_curl_prefix}")
	endif()
endif()

if(PkgConfig_FOUND)
	if(EDWL_STATIC_CURL)
		set(_edwl_pkg_mode "--static")
	else()
		set(_edwl_pkg_mode "")
	endif()

	execute_process(
		COMMAND ${PKG_CONFIG_EXECUTABLE} ${_edwl_pkg_mode} --cflags libcurl
		OUTPUT_VARIABLE _edwl_curl_cflags
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE _edwl_curl_rc
	)
	execute_process(
		COMMAND ${PKG_CONFIG_EXECUTABLE} ${_edwl_pkg_mode} --libs libcurl
		OUTPUT_VARIABLE _edwl_curl_libs
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	execute_process(
		COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion libcurl
		OUTPUT_VARIABLE EDWL_CURL_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)

	if(_edwl_curl_rc STREQUAL "0" AND _edwl_curl_libs)
		separate_arguments(_edwl_curl_cflags NATIVE_COMMAND "${_edwl_curl_cflags}")
		separate_arguments(_edwl_curl_libs NATIVE_COMMAND "${_edwl_curl_libs}")
		set(EDWL_CURL_CFLAGS ${_edwl_curl_cflags})
		set(EDWL_CURL_LIBS ${_edwl_curl_libs})
		set(EDWL_CURL_FOUND TRUE)
		set(EDWL_CURL_BACKEND "pkg-config")
	endif()
endif()

# ---------------------------------------------------------------------------
# 核心静态库（不含 main，测试直接复用）
# ---------------------------------------------------------------------------

add_library(edwl_lib STATIC ${EDWL_LIB_SOURCES})
target_include_directories(edwl_lib PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(EDWL_CURL_FOUND)
	target_include_directories(edwl_lib PUBLIC ${EDWL_CURL_CFLAGS})
	target_link_libraries(edwl_lib PUBLIC ${EDWL_CURL_LIBS})
else()
	find_package(CURL REQUIRED)
	target_include_directories(edwl_lib PUBLIC ${CURL_INCLUDE_DIRS})
	target_link_libraries(edwl_lib PUBLIC ${CURL_LIBRARIES})
	if(CURL_VERSION_STRING)
		set(EDWL_CURL_VERSION ${CURL_VERSION_STRING})
	endif()
	set(EDWL_CURL_BACKEND "find_package")
endif()

if(MSVC)
	target_compile_options(edwl_lib PRIVATE /W4)
else()
	target_compile_options(edwl_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
	if(EDWL_STATIC_CURL)
		# curl.h 在静态链接时必须知道不该用 __declspec(dllimport)
		target_compile_definitions(edwl_lib PUBLIC CURL_STATICLIB)
		target_link_options(edwl_lib PUBLIC -static -static-libgcc)
	endif()
else()
	target_compile_definitions(edwl_lib PRIVATE _FILE_OFFSET_BITS=64)
endif()

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

add_executable(edwl src/main.c)
target_link_libraries(edwl PRIVATE edwl_lib)

install(TARGETS edwl RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------

if(EDWL_BUILD_TESTS)
	enable_testing()
	foreach(tname test_url test_segment test_queue test_checksum test_file_io)
		add_executable(${tname} tests/${tname}.c)
		target_link_libraries(${tname} PRIVATE edwl_lib)
		target_include_directories(${tname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
		add_test(NAME ${tname} COMMAND ${tname})
	endforeach()
endif()

message(STATUS "edwl: libcurl 后端 = ${EDWL_CURL_BACKEND}")
message(STATUS "edwl: libcurl 版本 = ${EDWL_CURL_VERSION}")
message(STATUS "edwl: 静态链接 libcurl = ${EDWL_STATIC_CURL}")

# 许可证提醒：静态链接会把 LGPL 组件一起塞进 exe，分发前需要合规动作。
if(EDWL_STATIC_CURL AND EDWL_CURL_LIBS)
	string(FIND "${EDWL_CURL_LIBS}" "idn2" _edwl_idn2_pos)
	if(NOT _edwl_idn2_pos EQUAL -1)
		message(WARNING
			"静态链接包含 LGPL 许可的 libidn2/libunistring（可能还有 libiconv）。"
			"分发生成的二进制之前，请先读 THIRD_PARTY_NOTICES.md")
	endif()
endif()
