/**
 * @file water_quality_ml.h
 * @brief 水质 1D-CNN 推理服务（AWQ-10k / pool168_s42，4 通道 × 24 步）
 *
 * 模型输入（原始工程量，内部用权重里自带的 scaler 标准化）：
 *   [0] 水温 °C    [1] pH    [2] TDS ppm    [3] 浊度 NTU
 * 窗口：最近 WQ_WINDOW(24) 个采样点，按 wq_step_ms 的间隔节流取样，
 *       即默认 24 × 2 min = 48 min 的滑动窗口（训练集用的是 24 × 30 min，
 *       采样节拍不同，见 ML,STEP 命令与集成说明里的注意事项）。
 * 输出：4 类概率 + argmax
 *   0 Normal 正常 / 1 Caution 注意 / 2 Warning 警告 / 3 Severe 严重
 *
 * 数据不足、探头无效或数据过期时不会硬跑模型：结果是 ok=false + status 说明，
 * 避免"用旧值凑一个看似正常的结论"。
 */
#ifndef WATER_QUALITY_ML_H
#define WATER_QUALITY_ML_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WQ_CLASS_COUNT 4

/** 最近一次推理结果的快照（结构体整体拷贝，读侧无锁） */
typedef struct {
    bool ok;                     /**< 本次推理是否有效（窗口满 + 输入新鲜） */
    bool enabled;                 /**< 推理开关（ML,OFF 关闭） */
    int cls;                      /**< 0..3，ok=false 时恒为 -1 */
    float prob[WQ_CLASS_COUNT];   /**< 4 类概率，和恒为 1 */
    uint32_t updated_ms;          /**< 本次推理的运行时间(ms) */
    uint32_t infer_us;            /**< 推理耗时(us) */
    uint8_t samples;              /**< 窗口内有效样本数 0..24 */
    bool window_full;             /**< 窗口是否已满 */
    float window_span_s;          /**< 窗口实际覆盖时长(秒) */
    float step_s;                 /**< 当前采样节拍(秒) */
    bool feat_valid[4];           /**< 最近一帧各通道是否有效 */
    float feat[4];                /**< 最近一帧原始值（无效通道为最后有效值或 0） */
    char status[12];              /**< "OK"/"WARMUP"/"NO_DATA"/"STALE"/"OFF"/"INIT" */
} wq_result_t;

/** 初始化（只清状态，不创建任务） */
esp_err_t water_quality_init(void);

/** 创建推理任务（1s 轮询：节流采样 + 周期推理） */
esp_err_t water_quality_start(void);

/** 读最近一次结果快照；返回 false 表示服务尚未启动过 */
bool water_quality_get(wq_result_t *out);

/** 请求立即推理并等待结果（最长 timeout_ms，典型 <100ms） */
esp_err_t water_quality_predict(wq_result_t *out, uint32_t timeout_ms);

/** 手工塞一帧样本（探头坏了也能验证模型打通；不在工程量范围内返回错误） */
esp_err_t water_quality_feed(float temp_c, float ph, float tds_ppm, float turb_ntu);

/** 清空滑动窗口 */
void water_quality_reset(void);

/** 采样节拍（毫秒），约束在 [10s, 3600s] */
void water_quality_set_step_ms(uint32_t step_ms);
uint32_t water_quality_step_ms(void);

/** 推理开关 */
void water_quality_set_enabled(bool enabled);
bool water_quality_enabled(void);

const char *water_quality_class_name(int cls);   /**< "Normal" */
const char *water_quality_class_label(int cls);  /**< "正常" */

#ifdef __cplusplus
}
#endif

#endif // WATER_QUALITY_ML_H
