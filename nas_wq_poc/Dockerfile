# NAS 侧只跑推理，不需要 GPU / 不需要 PyTorch。
# 镜像 ~180 MB（python:3.12-slim + numpy + paho）。
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    WQ_DB=/data/wq_nas.sqlite \
    WQ_STEP_S=1800

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir numpy "paho-mqtt>=2.0"

COPY wq_nas/ ./wq_nas/
COPY run_live.py weights.json ./

RUN mkdir -p /data && useradd -r -u 10001 wq && chown -R wq /data /app
USER wq

# 健康检查：进程还活着且 DB 可读
HEALTHCHECK --interval=60s --timeout=5s --start-period=20s --retries=3 \
  CMD python -c "import sqlite3,os,sys; \
      p=os.environ['WQ_DB']; \
      sys.exit(0 if (not os.path.exists(p)) or sqlite3.connect(p).execute('select 1').fetchone() else 1)"

ENTRYPOINT ["python", "run_live.py"]
