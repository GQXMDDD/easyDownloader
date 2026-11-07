/*
 * edwl - 配置文件（edwl.ini）
 *
 * 结构对应 docs/config.md 里的模板；解析用 inih（third_party/inih）。
 * 优先级：命令行参数 > 配置文件 > 内置默认值。
 *
 * 注意：本模块只做"读进结构体 / 写默认模板"，不负责把值映射到运行参数，
 * 那部分在 main.c 里做（便于看出哪一项来自哪里）。
 */
#ifndef EDWL_CORE_CONFIG_H
#define EDWL_CORE_CONFIG_H

#include <stddef.h>

#define EDWL_CFG_PATH 256
#define EDWL_CFG_HOSTS 1024
#define EDWL_CFG_IPS 256

/* 镜像映射：[Mirrors] 段里的一行 */
struct edwl_mirror_map {
	char original_host[128];
	char mirrors[512];
	struct edwl_mirror_map *next;
};

struct edwl_config {
	/* [General] */
	int threads;	     /* 静态并发；动态线程时作为上限 */
	int dynamic_threads; /* 1 = 根据速率自动增减并发 */
	int max_speed;	     /* KB/s，0 表示不限 */
	int min_speed;	     /* KB/s，低于它且持续 Timeout 秒则放弃 */
	int timeout;	     /* 秒 */
	int retries;
	char retry_backoff[16]; /* exponential | fixed */
	char output_dir[EDWL_CFG_PATH];
	int chunk_size_mb;
	char user_agent[256];
	char resume_ext[16];

	/* [OpenSpeedy] 仅解析保存；edwl 不驱动外部变速工具（见 docs） */
	int openspeedy_enable;
	char openspeedy_multiplier[16];

	/* [Logging] */
	int log_enable;
	char log_level[16];  /* off | low | high | max */
	char log_output[16]; /* console | file | both */
	char log_filepath[EDWL_CFG_PATH];
	char log_retention[16]; /* 1000L（按行） | 30T（按天） */
	char log_time_format[64];

	/* [Blacklist] */
	int blacklist_enable;
	char blacklist_hosts[EDWL_CFG_HOSTS];
	char blacklist_ips[EDWL_CFG_IPS];

	struct edwl_mirror_map *mirrors;
};

/* 填入内置默认值（不读文件） */
void edwl_config_init_defaults(struct edwl_config *cfg);

/*
 * 读配置文件。返回：
 *   1  读取成功（部分字段可能有语法问题，会记日志）
 *   0  文件不存在（保持调用前的值）
 *  -1  解析出错或内存不足
 */
int edwl_config_load(struct edwl_config *cfg, const char *path);

/* 写一份带注释的默认模板。0 成功，-1 失败 */
int edwl_config_generate(const char *path);

/* 释放镜像链表 */
void edwl_config_free(struct edwl_config *cfg);

/*
 * 解析 [OpenSpeedy] SpeedMultiplier：
 *   "auto" → *is_auto = 1，*out = 0
 *   数字   → *is_auto = 0，*out = 倍率（0.25 ~ 8.0）
 * 0 成功，-1 无法识别。
 */
/*
 * 修改配置文件里的某一项（保留原有注释与顺序）：
 *   pair 形如 "Threads=16"、"Logging.Level=high"。
 * 文件不存在时先生成模板。返回 0 成功，-1 参数错，-2 未知键，-3 IO 错。
 */
int edwl_config_set_file(const char *path, const char *pair);

/* 某个裸键名出现在哪些段里（给 -set 的歧义提示用），返回段数 */
int edwl_config_key_sections(const char *key, char *out, size_t out_size);

int edwl_config_parse_multiplier(const char *text, double *out, int *is_auto);

/* 按原始域名找镜像行（大小写不敏感，精确匹配域名，不含端口） */
const struct edwl_mirror_map *
edwl_config_find_mirrors(const struct edwl_config *cfg, const char *host);

#endif /* EDWL_CORE_CONFIG_H */
