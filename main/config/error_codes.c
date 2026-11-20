#include "config/error_codes.h"

const char *app_err_name(esp_err_t err) {
    switch (err) {
        case ESP_OK:                     return "OK";
        case ESP_FAIL:                   return "FAIL";
        case ESP_ERR_NO_MEM:             return "NO_MEM";
        case ESP_ERR_INVALID_ARG:        return "INVALID_ARG";
        case ESP_ERR_INVALID_STATE:      return "INVALID_STATE";
        case ESP_ERR_INVALID_SIZE:       return "INVALID_SIZE";
        case ESP_ERR_INVALID_RESPONSE:   return "INVALID_RESPONSE";
        case ESP_ERR_NOT_FOUND:          return "NOT_FOUND";
        case ESP_ERR_NOT_SUPPORTED:      return "NOT_SUPPORTED";
        case ESP_ERR_TIMEOUT:            return "TIMEOUT";
        case ESP_ERR_INVALID_CRC:        return "INVALID_CRC";
        case APP_ERR_INVALID_STATE:      return "APP_INVALID_STATE";
        case APP_ERR_BAD_FRAME:          return "BAD_FRAME";
        case APP_ERR_UNKNOWN_CMD:        return "UNKNOWN_CMD";
        case APP_ERR_BAD_ARG:            return "BAD_ARG";
        case APP_ERR_UNKNOWN_ID:         return "UNKNOWN_ID";
        case APP_ERR_NOT_ALLOWED:        return "NOT_ALLOWED";
        case APP_ERR_OVERFLOW:           return "OVERFLOW";
        case APP_ERR_SENSOR_READ:        return "SENSOR_READ";
        case APP_ERR_SENSOR_STALE:       return "SENSOR_STALE";
        case APP_ERR_SENSOR_RANGE:       return "SENSOR_RANGE";
        case APP_ERR_ACTUATOR:           return "ACTUATOR";
        case APP_ERR_ACTUATOR_COOLDOWN:  return "ACTUATOR_COOLDOWN";
        case APP_ERR_NVS:                return "NVS";
        case APP_ERR_NVS_BLOB:           return "NVS_BLOB";
        case APP_ERR_NVS_CRC:            return "NVS_CRC";
        case APP_ERR_IO:                 return "IO";
        default:                         return "UNKNOWN";
    }
}

const char *app_err_desc(esp_err_t err) {
    switch (err) {
        case ESP_OK:                     return "成功";
        case ESP_FAIL:                   return "操作失败";
        case ESP_ERR_NO_MEM:             return "内存不足";
        case ESP_ERR_INVALID_ARG:        return "参数非法";
        case ESP_ERR_INVALID_STATE:      return "状态非法(模块未初始化)";
        case ESP_ERR_NOT_FOUND:          return "设备未找到";
        case ESP_ERR_NOT_SUPPORTED:      return "硬件不支持";
        case ESP_ERR_TIMEOUT:            return "超时";
        case ESP_ERR_INVALID_CRC:        return "校验错误";
        case APP_ERR_BAD_FRAME:          return "帧格式错误";
        case APP_ERR_UNKNOWN_CMD:        return "未知命令";
        case APP_ERR_BAD_ARG:            return "命令参数非法";
        case APP_ERR_UNKNOWN_ID:         return "未知的ID或参数名";
        case APP_ERR_NOT_ALLOWED:        return "当前模式不允许该操作";
        case APP_ERR_OVERFLOW:           return "数据溢出";
        case APP_ERR_SENSOR_READ:        return "传感器读取失败";
        case APP_ERR_SENSOR_STALE:       return "传感器数据过期";
        case APP_ERR_SENSOR_RANGE:       return "传感器读数超量程";
        case APP_ERR_ACTUATOR:           return "执行器操作失败";
        case APP_ERR_ACTUATOR_COOLDOWN:  return "执行器冷却中";
        case APP_ERR_NVS:                return "NVS 操作失败";
        case APP_ERR_NVS_BLOB:           return "NVS 数据损坏";
        case APP_ERR_NVS_CRC:            return "NVS 校验失败";
        case APP_ERR_IO:                 return "文件/IO 操作失败";
        default:                         return "未知错误";
    }
}
