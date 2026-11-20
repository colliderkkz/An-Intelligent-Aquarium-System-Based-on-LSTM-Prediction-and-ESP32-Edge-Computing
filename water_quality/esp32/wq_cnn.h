#ifndef WQ_CNN_H
#define WQ_CNN_H

#ifdef __cplusplus
extern "C" {
#endif

/* 窗口/通道/类别数：真实取值在 wq_cnn_weights.h 里（由 export_c.py 生成）。
 * 这里用 #ifndef 兜底，调用方（如 ml/water_quality_ml.c）不必为了拿这几个
 * 常量去解析 280 KB 的权重头文件；wq_cnn.c 里还有 _Static_assert 防止两者漂移。 */
#ifndef WQ_WINDOW
#define WQ_WINDOW 24
#endif
#ifndef WQ_IN_CHANNELS
#define WQ_IN_CHANNELS 4
#endif
#ifndef WQ_CLASSES
#define WQ_CLASSES 4
#endif

/* Run one inference.
 * window: WQ_WINDOW x WQ_IN_CHANNELS raw sensor samples, oldest first.
 *         channel order: temp_c, ph, tds_mg_l, turbidity_ntu
 * prob:   output class probabilities (Normal, Caution, Warning, Severe)
 * return: argmax class 0..3
 */
int wq_cnn_predict(const float window[24][4], float prob[4]);

#ifdef __cplusplus
}
#endif

#endif /* WQ_CNN_H */
