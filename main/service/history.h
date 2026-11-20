#ifndef HISTORY_H
#define HISTORY_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 SD 卡的传感器 CSV 里读最近的采样，生成紧凑 JSON
 *
 * 输出格式（最多 want 个点，自动降采样）：
 *   {"type":"hist","step":0,"n":120,"v":[[ago,26.5,7.12,55.0,30.1],...]}
 * v 的每一项是 [距今多少秒, 水温, pH, 水位, 光照]，无效值为 -1，按时间从旧到新。
 * 手机端把最后一点对齐到"现在"，整条曲线的时间轴就是准的。
 *
 * 性能说明：SD 卡在这块板子上读得很慢（实测 48KB 要 9 秒），
 * 所以第一次调用会把解析结果放进 RAM 缓存，之后所有请求都是
 * 毫秒级返回；新的采样由 history_note_sample() 持续补进来。
 *
 * @param out      输出缓冲
 * @param cap      缓冲大小
 * @param want     期望点数
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 没数据；ESP_ERR_NO_MEM 缓冲不够
 */
esp_err_t history_build_json(char *out, size_t cap, int want);

/**
 * @brief 记一个实时采样点（掉线也要记，曲线才连续）
 *
 * 内部按 20 秒最小间隔抽稀，同一窗口内只刷新数值不新增点。
 * 无有效值的通道传负数（-1）。
 */
void history_note_sample(float temp, float ph, float level, float light);

#ifdef __cplusplus
}
#endif

#endif // HISTORY_H
