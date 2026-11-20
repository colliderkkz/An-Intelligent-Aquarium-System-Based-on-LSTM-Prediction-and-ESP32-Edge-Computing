# 水质 AI 模型 · NAS 推理服务（可行性验证 PoC）

把 `水质AI模型_交付包_v1.0.zip` 里的 1D-CNN（`pool168_s42`，16,292 参数）挂到 NAS 上跑推理，
通过 MQTT 把判定结果推给手机 App。**不修改固件，手机不再打包 87 KB 权重。**

配套论证报告：`../水质AI模型_NAS+MQTT可行性论证.md`（DOCX 同目录）。

---

## 1. 它做什么

```
ESP32-S3 ──publish──> Broker ──subscribe──> 本服务 ──推理──> ──publish retained──> App
            <base>/state                     (24 点窗口)         <base>/wq/nas
                                                │
                                              SQLite（全分辨率原始帧 + 判定历史）
```

- **输入**：订阅既有 `state` 主题，字段 `t / ph / tds / tur`（支持 5 组别名与三种嵌套包裹）
- **窗口**：按 `ts` 分桶采样，`step_s` 默认 **1800 s**（= 训练口径 12 h 窗口，不是固件默认的 120 s）
- **输出**：`status` + `cls` + 4 类概率 + 置信度 + 投票一致性 + 窗口覆盖时长 + 推理耗时
- **可靠性**：3 帧多数投票、低置信度标记、`NO_DATA/STALE` 降级、重启从 DB 重建窗口、LWT 掉线可见

---

## 2. 三条硬约束（踩过坑才有）

| 约束 | 原因 |
|---|---|
| `step_s = 1800` | 实测：比 120 s 口径**首报早 9.5 h**，投喂尖峰误报 **17 次 → 0 次** |
| 结果**必须 retained** | 否则手机冷启动时可能屏幕全空 |
| `status != OK` 时 `cls` 必须为 `null` | 否则 App 读 `cls` 会把"探头掉了"渲染成"正常"（本项目实际踩到过） |

---

## 3. 本地验证（不需要真机、不需要 NAS）

```bash
pip install -r requirements.txt     # numpy / paho-mqtt / amqtt(仅测试)

python tests/test_parity.py   # 与包内官方 NumPy 参考实现对拍 -> 0/2000 不一致
python footprint.py           # 资源占用 -> 0.51 ms/次，RSS 39.5 MiB
python compare_cadence.py     # 节拍对照 -> 1800 s vs 600 s vs 120 s
python run_e2e.py             # 端到端（真实 MQTT）-> 13 项全绿
```

`run_e2e.py` 会自己起一个 `amqtt` broker（真实 MQTT 3.1.1），跑完自动关。
覆盖：连接/订阅、WARMUP、OK 判定、retained 迟到订阅、heartbeat、availability、
**进程重启后窗口恢复**、**探头掉线降级为 NO_DATA**。

---

## 4. 部署到 NAS

### 4.1 最简（已有 Mosquitto）

```bash
docker build -t wq-nas:1.0.0 .
docker run -d --name wq-nas --restart unless-stopped \
  -e WQ_MQTT_HOST=192.168.1.10 \
  -e WQ_BASE=pool/01 \
  -e WQ_STEP_S=1800 \
  -v wq-data:/data \
  wq-nas:1.0.0
```

### 4.2 compose

```yaml
services:
  mosquitto:
    image: eclipse-mosquitto:2
    restart: unless-stopped
    ports: ["1883:1883"]
    volumes:
      - ./mosquitto.conf:/mosquitto/config/mosquitto.conf:ro
      - mosquitto-data:/mosquitto/data
    volumes: ...
  wq-nas:
    build: .
    restart: unless-stopped
    depends_on: [mosquitto]
    environment:
      WQ_MQTT_HOST: mosquitto
      WQ_BASE: pool/01
      WQ_STEP_S: "1800"
    volumes: [wq-data:/data]
```

完整版见 `docker-compose.yml`。

### 4.3 群晖 / 威联通 / TrueNAS

Container Manager 里导入 `docker-compose.yml` 即可；把 `/data` 映射到共享文件夹（例如 `docker/wq-nas/data`）。

### 4.4 不用 Docker

```bash
python run_live.py --host 192.168.1.10 --base pool/01 --step-s 1800 --db data/wq_nas.sqlite
```
配 systemd / 计划任务开机自启。

---

## 5. 配置项（环境变量或命令行）

| 变量 | 默认 | 说明 |
|---|---|---|
| `WQ_MQTT_HOST` | `127.0.0.1` | broker 地址 |
| `WQ_MQTT_PORT` | `1883` | 明文端口；TLS 用 8883 需自行加证书 |
| `WQ_MQTT_USER` / `WQ_MQTT_PASS` | — | 生产必须设置 |
| `WQ_BASE` | `pool/01` | 主题前缀 |
| `WQ_STATE_TOPIC` | `<base>/state` | 原始帧主题（**按现场实际改**） |
| `WQ_RESULT_TOPIC` | `<base>/wq/nas` | 判定结果主题 |
| `WQ_STEP_S` | `1800` | 窗口步长（秒），见 §2 |
| `WQ_VOTE_N` | `3` | 多数投票帧数 |
| `WQ_MIN_CONF` | `0.6` | 低于此置 `conf_ok=false` |
| `WQ_HEARTBEAT_S` | `30` | 心跳周期 |
| `WQ_DB` | `data/wq_nas.sqlite` | SQLite 路径 |
| `WQ_WEIGHTS` | `weights.json` | 权重文件 |

> **上线前必须先确认 `WQ_STATE_TOPIC` 与字段名。** 交付包只说明了 App 的 MQTT `state`
> 回调里有 `t/ph/tds/tur`，没有给出主题全名。本服务已支持别名与嵌套包裹，
> 现场通常只需改这一处。

---

## 6. 载荷契约

**输入**（固件原有，本服务只读）：

```json
{"ts": 1800000000, "t": 25.1, "ph": 7.21, "tds": 205.3, "tur": 5.4}
```

**输出**（301 B，retained）：

```json
{"ts":1800115200,"seq":65,"src":"nas","model":"pool168_s42","status":"OK",
 "cls":2,"name":"Warning","label":"警告","cls_raw":2,
 "p":[0.0075,0.0226,0.9097,0.0601],"conf":0.9097,"conf_ok":true,
 "vote_agree":3,"stable":true,"n":24,"step_s":1800,"span_s":41400.0,
 "in_dist":true,"infer_ms":0.556,"notes":[]}
```

`status` 取值：`INIT / WARMUP / OK / STALE / NO_DATA`。**非 `OK` 时 `cls`、`name`、
`label`、`conf`、`p` 全部为 `null`** —— App 必须先判 `status`，不能直接读 `cls`。

`in_dist=false` 表示某个通道超出训练范围（`notes` 会指出是哪个），此时结果仅供参考。

**心跳**（`<base>/wq/nas/health`，retained）：窗口填充度 `n/need`、`span_s`、
`last_sample_age_s`、帧计数、DB 计数。用于区分"水质正常"与"服务/探头挂了"。

**在线状态**（`<base>/availability`，retained + LWT）：`{"online": true|false}`。

---

## 7. App 侧接入要点

1. 订阅三个主题：`wq/nas`、`wq/nas/health`、`availability`；
2. 渲染优先级：`availability.online == false` → "NAS 离线"；`status != OK` → 显示对应提示；否则显示判定；
3. `conf_ok == false` 时提示"观察中"；`stable == false` 时提示"投票中"；
4. **不要**再本地算一遍（`wq_cnn.js`）后与 NAS 结果混用；
5. 建议 UI 上标注"来源：NAS"，并让固件 `state.wq` 下线，只留一个数据源。

---

## 8. 目录

```
nas_wq_poc/
├─ wq_nas/
│  ├─ model.py     权重加载 + 向量化推理 + 量程/分布校验
│  ├─ engine.py    分桶采样、窗口、3 帧投票、状态机、结果序列化
│  ├─ store.py     SQLite（raw 原始帧 + result 判定历史 + 重启重建）
│  └─ service.py   MQTT 订阅/发布、LWT、心跳、状态变更推送
├─ tools/
│  ├─ dev_broker.py  内嵌 amqtt broker（仅测试）
│  ├─ sensor_sim.py  传感器轨迹生成 + 发布（6 种场景）
│  └─ app_sim.py     手机侧订阅与渲染
├─ tests/test_parity.py
├─ run_e2e.py / run_live.py / compare_cadence.py / footprint.py
├─ Dockerfile / docker-compose.yml / requirements.txt / weights.json
```

---

## 9. 已知限制

- 本服务**不改变模型精度**。交付模型在真实鱼缸上的有效性未经证实
  （真实鱼塘零样本 34.9%，且 69.6% 判为 Severe）。请先按报告 §7.1 阶段 1「静默观察」上线。
- 需要在现场确认 `state` 主题名与字段名。
- 只发布判定，**不订阅控制主题、不驱动执行器**。
- 生产环境禁止把 broker 或 NAS 端口映射到公网。
