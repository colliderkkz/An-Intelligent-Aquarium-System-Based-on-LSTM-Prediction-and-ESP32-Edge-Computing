/**
 * @file control_mode.h
 * @brief AUTO / MANUAL 模式管理
 *
 * 现场调试时，上位机需要能直接点动任意执行器；但自动控制任务（温控/补水/
 * 灯光/喂食/紫外）如果同时又在按自己的策略写同一个引脚，就会互相打架。
 * 解决办法：一旦上位机发来任何手动动作，就进入 MANUAL 模式，所有自动控制
 * 任务直接空转；超时（默认 120s，可在参数表里改）没有新的手动命令则自动
 * 回到 AUTO，并在下一个控制周期把执行器恢复到自动策略，避免“忘了退回自动”。
 */
#ifndef CONTROL_MODE_H
#define CONTROL_MODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void control_mode_init(void);

/** 是否处于手动模式（自动控制任务据此跳过输出） */
bool control_mode_is_manual(void);

/** 手动模式剩余秒数（0 表示已不在手动） */
uint32_t control_mode_manual_left_s(void);

/** 进入/刷新手动模式，timeout_s 为 0 时使用参数表里的默认值 */
void control_mode_enter_manual(uint32_t timeout_s);

/** 立即返回自动模式 */
void control_mode_exit_manual(void);

/** 手动模式剩余秒数（非手动时为 0） */
uint32_t control_mode_manual_remaining_s(void);

/** "AUTO" / "MANUAL" */
const char *control_mode_name(void);

/** 自动控制总开关（对应参数 auto_enable） */
bool control_mode_auto_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_MODE_H
