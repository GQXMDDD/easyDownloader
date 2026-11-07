/*
 * edwl - 平台抽象层：线程、互斥量、条件变量
 *
 * 三个类型都是不透明结构体，定义只在 platform/{posix,win32}/ 的 .c 里。
 * 核心层永远不知道底层是 pthread_t 还是 HANDLE。
 */
#ifndef EDWL_PLATFORM_THREAD_H
#define EDWL_PLATFORM_THREAD_H

#include <stdint.h>

typedef struct edwl_thread edwl_thread;
typedef struct edwl_mutex edwl_mutex;
typedef struct edwl_cond edwl_cond;

/* 线程入口：返回值为空指针即可，调用方不回收 */
typedef void *(*edwl_thread_fn)(void *arg);

/* 创建并启动线程。0 成功，-1 失败 */
int edwl_thread_create(edwl_thread **out, edwl_thread_fn fn, void *arg);

/* 等待线程结束并释放线程对象。0 成功，-1 失败 */
int edwl_thread_join(edwl_thread *t);

/* 取当前线程的系统标识，仅用于日志等展示用途 */
unsigned long edwl_thread_current_id(void);

int edwl_mutex_init(edwl_mutex **out);
int edwl_mutex_lock(edwl_mutex *m);
int edwl_mutex_unlock(edwl_mutex *m);
void edwl_mutex_destroy(edwl_mutex *m);

int edwl_cond_init(edwl_cond **out);
int edwl_cond_wait(edwl_cond *c, edwl_mutex *m);
/* 带超时等待（毫秒），超时返回 0，被唤醒返回 1，失败 -1 */
int edwl_cond_timedwait(edwl_cond *c, edwl_mutex *m, uint32_t ms);
int edwl_cond_signal(edwl_cond *c);
int edwl_cond_broadcast(edwl_cond *c);
void edwl_cond_destroy(edwl_cond *c);

#endif /* EDWL_PLATFORM_THREAD_H */
