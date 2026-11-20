#include "wq_cnn.h"

/* 权重头文件由 tools/export_c.py 生成，里面还带着几个仅用于自描述的
 * *_shape 数组，本文件不引用；不屏蔽会刷一堆 -Wunused-const-variable。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "wq_cnn_weights.h"
#pragma GCC diagnostic pop

#include <math.h>
#include <string.h>

/* 权重头文件里的维度必须和 wq_cnn_predict() 写死的 24 x 4 -> 4 类一致，
 * 否则说明导出时换了窗口长度/类别数，下面的实现就不再适用。 */
_Static_assert(WQ_WINDOW == 24, "wq_cnn_predict() assumes a 24-step window");
_Static_assert(WQ_IN_CHANNELS == 4, "wq_cnn_predict() assumes 4 input channels");
_Static_assert(WQ_CLASSES == 4, "wq_cnn_predict() assumes 4 classes");

static void conv_relu(const float *in, int cin, int lin,
                      const float *w, const float *b, int cout, int k, int pad,
                      float *out, int lout)
{
    for (int oc = 0; oc < cout; oc++) {
        for (int t = 0; t < lout; t++) {
            float acc = b[oc];
            for (int ic = 0; ic < cin; ic++) {
                const float *wp = w + ((oc * cin) + ic) * k;
                const float *ip = in + ic * lin;
                for (int kk = 0; kk < k; kk++) {
                    int pos = t + kk - pad;
                    if (pos >= 0 && pos < lin) acc += ip[pos] * wp[kk];
                }
            }
            out[oc * lout + t] = acc > 0.0f ? acc : 0.0f;
        }
    }
}

static void maxpool2(const float *in, int c, int lin, float *out, int lout)
{
    for (int oc = 0; oc < c; oc++) {
        const float *ip = in + oc * lin;
        float *op = out + oc * lout;
        for (int t = 0; t < lout; t++) {
            float a = ip[2 * t], b = ip[2 * t + 1];
            op[t] = a > b ? a : b;
        }
    }
}

int wq_cnn_predict(const float window[24][4], float prob[4])
{
    float x[4][24];
    for (int c = 0; c < 4; c++)
        for (int t = 0; t < 24; t++)
            x[c][t] = (window[t][c] - wq_scaler_mean[c]) / wq_scaler_std[c];

    float h1[32 * 24], h2[32 * 24], p2[32 * 12], h3[64 * 12];
    conv_relu(&x[0][0], 4, 24, conv1_w, conv1_b, 32, 5, 2, h1, 24);
    conv_relu(h1, 32, 24, conv2_w, conv2_b, 32, 5, 2, h2, 24);
    maxpool2(h2, 32, 24, p2, 12);
    conv_relu(p2, 32, 12, conv3_w, conv3_b, 64, 3, 1, h3, 12);

    float pooled[128];
    for (int oc = 0; oc < 64; oc++) {
        float s = 0.0f;
        for (int t = 0; t < 12; t++) s += h3[oc * 12 + t];
        pooled[oc] = s / 12.0f;
        pooled[64 + oc] = h3[oc * 12 + 11];
    }

    float z[32];
    for (int j = 0; j < 32; j++) {
        float acc = fc1_b[j];
        for (int i = 0; i < 128; i++) acc += fc1_w[j * 128 + i] * pooled[i];
        z[j] = acc > 0.0f ? acc : 0.0f;
    }

    float logits[4], mx = -1e30f;
    for (int j = 0; j < 4; j++) {
        float acc = fc2_b[j];
        for (int i = 0; i < 32; i++) acc += fc2_w[j * 32 + i] * z[i];
        logits[j] = acc;
        if (acc > mx) mx = acc;
    }
    float sum = 0.0f;
    for (int j = 0; j < 4; j++) { prob[j] = expf(logits[j] - mx); sum += prob[j]; }
    for (int j = 0; j < 4; j++) prob[j] /= sum;

    int best = 0;
    for (int j = 1; j < 4; j++) if (prob[j] > prob[best]) best = j;
    return best;
}
