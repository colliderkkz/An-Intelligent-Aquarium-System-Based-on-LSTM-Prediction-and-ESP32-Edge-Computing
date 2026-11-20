/*
 * 水质 1D-CNN 推理（纯 JavaScript，无依赖，浏览器 / WebView / Node 都可用）
 *
 * 与固件 main/ml/wq_cnn.c 逐算子一致：
 *   Conv1(4->32,k5,pad2) -> ReLU -> Conv2(32->32,k5,pad2) -> ReLU
 *   -> MaxPool2 -> Conv3(32->64,k3,pad1) -> ReLU
 *   -> [mean over 12, last] (128) -> FC(128->32) -> ReLU -> FC(32->4) -> softmax
 *
 * 用法：
 *   <script src="wq_weights.js"></script>   <!-- 定义 globalThis.WQ_WEIGHTS_B64 -->
 *   <script src="wq_cnn.js"></script>
 *   const r = WQ.predict(window24x4);
 *   // r = { cls: 0..3, probs: [p0..p3], name: 'Normal', label: '正常' }
 *
 * 输入：24 x 4 原始工程量，最旧在前，通道顺序 [水温℃, pH, TDS ppm, 浊度 NTU]。
 * 输出类别：0 Normal 正常 / 1 Caution 注意 / 2 Warning 警告 / 3 Severe 严重。
 */
(function (global) {
  "use strict";

  // 权重数组的物理顺序与长度（必须与 wq_cnn_weights.h 一致）
  const LENGTHS = {
    conv1_w: 640, conv1_b: 32,
    conv2_w: 5120, conv2_b: 32,
    conv3_w: 6144, conv3_b: 64,
    fc1_w: 4096, fc1_b: 32,
    fc2_w: 128, fc2_b: 4,
    wq_scaler_mean: 4, wq_scaler_std: 4
  };
  const ORDER = Object.keys(LENGTHS);
  const OFFSETS = {};
  let TOTAL = 0;
  for (const k of ORDER) { OFFSETS[k] = TOTAL; TOTAL += LENGTHS[k]; }

  const CLASS_NAMES = ["Normal", "Caution", "Warning", "Severe"];
  const CLASS_LABELS = ["正常", "注意", "警告", "严重"];

  let W = null;

  function decodeB64(b64) {
    // 浏览器 / WebView / Node 16+ 都有 atob
    const bin = atob(b64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new Float32Array(bytes.buffer);
  }

  function init(b64) {
    if (typeof b64 !== "string" || b64.length < 1000) {
      throw new Error("WQ: 权重 base64 无效");
    }
    const w = decodeB64(b64);
    if (w.length !== TOTAL) {
      throw new Error("WQ: 权重长度 " + w.length + " != " + TOTAL);
    }
    W = w;
    return true;
  }

  function ensure() {
    if (!W) {
      if (typeof global.WQ_WEIGHTS_B64 === "string") {
        init(global.WQ_WEIGHTS_B64);
      } else {
        throw new Error("WQ: 还没有 init()，也没有加载 wq_weights.js");
      }
    }
  }

  function convRelu(inp, cin, lin, w, wOff, bOff, cout, k, pad, out, lout) {
    for (let oc = 0; oc < cout; oc++) {
      for (let t = 0; t < lout; t++) {
        let acc = w[bOff + oc];
        for (let ic = 0; ic < cin; ic++) {
          const wp = wOff + (oc * cin + ic) * k;
          const ip = ic * lin;
          for (let kk = 0; kk < k; kk++) {
            const pos = t + kk - pad;
            if (pos >= 0 && pos < lin) {
              acc += inp[ip + pos] * w[wp + kk];
            }
          }
        }
        out[oc * lout + t] = acc > 0 ? acc : 0;
      }
    }
  }

  function predict(window) {
    ensure();

    let flat;
    if (Array.isArray(window) && window.length === 24 && Array.isArray(window[0])) {
      flat = [];
      for (let t = 0; t < 24; t++) {
        flat.push(window[t][0], window[t][1], window[t][2], window[t][3]);
      }
    } else if (window && window.length === 96) {
      flat = window;
    } else {
      throw new Error("WQ: 输入必须是 24x4 二维数组或长度 96 的扁平数组");
    }

    const mOff = OFFSETS.wq_scaler_mean;
    const sOff = OFFSETS.wq_scaler_std;
    const x = new Float32Array(4 * 24);
    for (let c = 0; c < 4; c++) {
      const m = W[mOff + c], s = W[sOff + c];
      for (let t = 0; t < 24; t++) {
        x[c * 24 + t] = (Number(flat[t * 4 + c]) - m) / s;
      }
    }

    const h1 = new Float32Array(32 * 24);
    convRelu(x, 4, 24, W, OFFSETS.conv1_w, OFFSETS.conv1_b, 32, 5, 2, h1, 24);

    const h2 = new Float32Array(32 * 24);
    convRelu(h1, 32, 24, W, OFFSETS.conv2_w, OFFSETS.conv2_b, 32, 5, 2, h2, 24);

    const p2 = new Float32Array(32 * 12);
    for (let c = 0; c < 32; c++) {
      for (let t = 0; t < 12; t++) {
        const a = h2[c * 24 + 2 * t];
        const b = h2[c * 24 + 2 * t + 1];
        p2[c * 12 + t] = a > b ? a : b;
      }
    }

    const h3 = new Float32Array(64 * 12);
    convRelu(p2, 32, 12, W, OFFSETS.conv3_w, OFFSETS.conv3_b, 64, 3, 1, h3, 12);

    const pooled = new Float32Array(128);
    for (let oc = 0; oc < 64; oc++) {
      let sum = 0;
      for (let t = 0; t < 12; t++) sum += h3[oc * 12 + t];
      pooled[oc] = sum / 12;
      pooled[64 + oc] = h3[oc * 12 + 11];
    }

    const z = new Float32Array(32);
    for (let j = 0; j < 32; j++) {
      let acc = W[OFFSETS.fc1_b + j];
      for (let i = 0; i < 128; i++) {
        acc += W[OFFSETS.fc1_w + j * 128 + i] * pooled[i];
      }
      z[j] = acc > 0 ? acc : 0;
    }

    const logits = new Float32Array(4);
    let mx = -Infinity;
    for (let j = 0; j < 4; j++) {
      let acc = W[OFFSETS.fc2_b + j];
      for (let i = 0; i < 32; i++) {
        acc += W[OFFSETS.fc2_w + j * 32 + i] * z[i];
      }
      logits[j] = acc;
      if (acc > mx) mx = acc;
    }

    const probs = new Float32Array(4);
    let sum = 0;
    for (let j = 0; j < 4; j++) {
      probs[j] = Math.exp(logits[j] - mx);
      sum += probs[j];
    }
    for (let j = 0; j < 4; j++) probs[j] /= sum;

    let cls = 0;
    for (let j = 1; j < 4; j++) if (probs[j] > probs[cls]) cls = j;

    return {
      cls: cls,
      probs: Array.from(probs),
      name: CLASS_NAMES[cls],
      label: CLASS_LABELS[cls]
    };
  }

  global.WQ = {
    init: init,
    predict: predict,
    ready: function () { return !!W; },
    params: TOTAL - 8,   // 16,292 个可训练参数；末尾 8 个是 scaler 的 mean/std
    CLASS_NAMES: CLASS_NAMES,
    CLASS_LABELS: CLASS_LABELS
  };
})(typeof globalThis !== "undefined" ? globalThis : this);
