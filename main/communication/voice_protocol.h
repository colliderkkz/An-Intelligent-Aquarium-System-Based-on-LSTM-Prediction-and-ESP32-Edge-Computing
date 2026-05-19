#ifndef VOICE_PROTOCOL_H
#define VOICE_PROTOCOL_H

#include <stdint.h>

#define VOICE_CMD_LIGHT_ON    0x01
#define VOICE_CMD_LIGHT_OFF   0x02
#define VOICE_CMD_PUMP_ON     0x03
#define VOICE_CMD_PUMP_OFF    0x04
#define VOICE_CMD_HEATER_ON   0x05
#define VOICE_CMD_HEATER_OFF  0x06
#define VOICE_CMD_CO2_ON      0x07
#define VOICE_CMD_CO2_OFF     0x08
#define VOICE_CMD_FEED        0x09
#define VOICE_CMD_UV_ON       0x0A
#define VOICE_CMD_UV_OFF      0x0B

#define VOICE_CMD_QUERY_TEMP  0x10
#define VOICE_CMD_QUERY_PH    0x11
#define VOICE_CMD_QUERY_TDS   0x12
#define VOICE_CMD_QUERY_ALL   0x13

#endif // VOICE_PROTOCOL_H
