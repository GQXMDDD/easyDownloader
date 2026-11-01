/*
 * edwl - 平台抽象层：线程、互斥量、条件变量
 *
 * 三个类型都是不透明结构体，定义只在 platform/{posix,win32}/ 的 .c 里。
 * 核心层永远不知道底层是 pthread_t 还是 HANDLE。
 */
#ifndef WGETX_PLATFORM_THREAD_H
#define WGETX_PLATFORM_THREAD_H

#include <stdint.h>

typedef struct wgetx_thread wgetx_thread;
typedef struct wgetx_mutex wgetx_mutex;
typedef struct wgetx_cond wgetx_cond;

/* 线程入口：返回值为空指针即可，调用方不回收 */
typedef void *(*wgetx_thread_fn)(void *arg);

/* 创建并启动线程。0 成功，-1 失败 */
int wgetx_thread_create(wgetx_thread **out, wgetx_thread_fn fn, void *arg);

/* 等待线程结束并释放线程对象。0 成功，-1 失败 */
int wgetx_thread_join(wgetx_thread *t);

/* 取当前线程的系统标识，仅用于日志等展示用途 */
unsigned long wgetx_thread_current_id(void);

int wgetx_mutex_init(wgetx_mutex **out);
int wgetx_mutex_lock(wgetx_mutex *m);
int wgetx_mutex_unlock(wgetx_mutex *m);
void wgetx_mutex_destroy(wgetx_mutex *m);

int wgetx_cond_init(wgetx_cond **out);
int wgetx_cond_wait(wgetx_cond *c, wgetx_mutex *m);
/* 带超时等待（毫秒），超时返回 0，被唤醒返回 1，失败 -1 */
int wgetx_cond_timedwait(wgetx_cond *c, wgetx_mutex *m, uint32_t ms);
int wgetx_cond_signal(wgetx_cond *c);
int wgetx_cond_broadcast(wgetx_cond *c);
void wgetx_cond_destroy(wgetx_cond *c);

#endif /* WGETX_PLATFORM_THREAD_H */
