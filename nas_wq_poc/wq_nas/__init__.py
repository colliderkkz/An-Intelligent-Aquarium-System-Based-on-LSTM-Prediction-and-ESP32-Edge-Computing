"""水质 AI 模型 NAS 服务（可行性验证用参考实现）。"""
from .engine import EngineConfig, Result, WindowEngine
from .model import CLASS_LABELS, CLASS_NAMES, Model, check_frame
from .service import ServiceConfig, WQService
from .store import Store

__all__ = [
    "Model", "WindowEngine", "EngineConfig", "Result",
    "Store", "WQService", "ServiceConfig",
    "CLASS_NAMES", "CLASS_LABELS", "check_frame",
]
__version__ = "nas-1.0.0"
