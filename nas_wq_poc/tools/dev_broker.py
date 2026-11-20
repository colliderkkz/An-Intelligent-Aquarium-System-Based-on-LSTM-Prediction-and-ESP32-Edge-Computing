"""内嵌 MQTT broker（仅测试用）。

NAS 上生产部署应直接用 Mosquitto / EMQX / Home Assistant 内置 broker。
这里用 amqtt 起一个真实 MQTT 3.1.1 broker，让端到端验证走真实协议栈，
而不是 in-process 假总线。
"""
from __future__ import annotations

import asyncio
import logging
import socket
import threading
import time

import paho.mqtt.client as mqtt

log = logging.getLogger("dev_broker")


class DevBroker:
    def __init__(self, port: int = 18830, host: str = "127.0.0.1") -> None:
        self.host, self.port = host, port
        self._thread: threading.Thread | None = None
        self._error: list[str] = []

    def start(self, timeout: float = 10.0) -> None:
        from amqtt.broker import Broker

        cfg = {
            "listeners": {"default": {"type": "tcp",
                                      "bind": f"{self.host}:{self.port}"}},
            "sys_interval": 1,
            "plugins": {
                "amqtt.plugins.authentication.AnonymousAuthPlugin":
                    {"allow_anonymous": True},
                "amqtt.plugins.sys.broker.BrokerSysPlugin":
                    {"sys_interval": 1},
            },
        }

        def run() -> None:
            async def main() -> None:
                try:
                    await Broker(cfg).start()
                    while True:
                        await asyncio.sleep(1)
                except Exception as e:  # noqa: BLE001
                    self._error.append(repr(e))
            try:
                asyncio.run(main())
            except Exception as e:  # noqa: BLE001
                self._error.append(repr(e))

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

        # 用真实 MQTT 连接探活（不用裸 socket，避免 broker 报解码错误）
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                client_id="probe", protocol=mqtt.MQTTv311)
                c.connect(self.host, self.port, 5)
                c.disconnect()
                return
            except OSError:
                if self._error:
                    raise RuntimeError(f"broker failed: {self._error}")
                time.sleep(0.15)
        raise TimeoutError(f"broker did not come up on {self.port}")


def port_free(port: int, host: str = "127.0.0.1") -> bool:
    s = socket.socket()
    try:
        s.bind((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()
