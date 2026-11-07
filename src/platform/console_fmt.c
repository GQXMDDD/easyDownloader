/*
 * edwl - 进度行格式化实现
 */
#include <stdio.h>
#include <string.h>

#include "console_fmt.h"

void edwl_console_format_size(uint64_t bytes, char *buf, size_t size)
{
	static const char *const units[] = {"B",   "KiB", "MiB",
					    "GiB", "TiB", "PiB"};
	double value = (double)bytes;
	int unit = 0;

	while (value >= 1024.0 && unit < 5) {
		value /= 1024.0;
		unit++;
	}
	if (unit == 0)
		snprintf(buf, size, "%.0f %s", value, units[unit]);
	else
		snprintf(buf, size, "%.1f %s", value, units[unit]);
}

static void format_speed(uint64_t bps, char *buf, size_t size)
{
	char unit[20];

	if (bps == 0) {
		snprintf(buf, size, "--");
		return;
	}
	edwl_console_format_size(bps, unit, sizeof(unit));
	snprintf(buf, size, "%s/s", unit);
}

static void format_elapsed(uint64_t ms, char *buf, size_t size)
{
	uint64_t total_s = ms / 1000;

	if (total_s >= 3600)
		snprintf(buf, size, "%llu:%02llu:%02llu",
			 (unsigned long long)(total_s / 3600),
			 (unsigned long long)((total_s / 60) % 60),
			 (unsigned long long)(total_s % 60));
	else
		snprintf(buf, size, "%02llu:%02llu",
			 (unsigned long long)(total_s / 60),
			 (unsigned long long)(total_s % 60));
}

size_t edwl_console_format_progress(const struct edwl_progress *p, char *buf,
				    size_t size, int width)
{
	char done[32], total[32], speed[48], elapsed[24];
	char bar[80];
	double pct = 0.0;
	int bar_w;
	int filled;
	int i;
	int n;

	if (!p || !buf || size < 40)
		return 0;
	if (width < 40)
		width = 40;

	edwl_console_format_size(p->done, done, sizeof(done));
	if (p->total > 0) {
		edwl_console_format_size(p->total, total, sizeof(total));
		pct = (double)p->done * 100.0 / (double)p->total;
		if (pct > 100.0)
			pct = 100.0;
	} else {
		snprintf(total, sizeof(total), "?");
	}
	format_speed(p->speed_bps, speed, sizeof(speed));
	format_elapsed(p->elapsed_ms, elapsed, sizeof(elapsed));

	/* 固定部分约占 46 列，剩下的给进度条 */
	bar_w = width - 46;
	if (bar_w < 8)
		bar_w = 8;
	if (bar_w > 40)
		bar_w = 40;

	filled = (int)(pct * (double)bar_w / 100.0 + 0.5);
	if (filled > bar_w)
		filled = bar_w;
	if (filled < 0)
		filled = 0;

	for (i = 0; i < bar_w; i++)
		bar[i] = (i < filled) ? '#' : '-';
	bar[bar_w] = '\0';

	if (p->segs_total > 0)
		n = snprintf(buf, size, "[%s] %5.1f%% %s/%s %s %s %u/%u", bar,
			     pct, done, total, speed, elapsed, p->segs_done,
			     p->segs_total);
	else
		n = snprintf(buf, size, "[%s] %5.1f%% %s/%s %s %s", bar, pct,
			     done, total, speed, elapsed);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		n = (int)size - 1;
	if (n > width)
		n = width;
	buf[n] = '\0';
	return (size_t)n;
}
