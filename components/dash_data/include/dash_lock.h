/**
 * @file dash_lock.h
 * 看板各模块之间共享状态的跨核互斥锁。
 *
 * ⚠️ 为什么不能用 portENTER_CRITICAL(&mux):
 * ESP-IDF v6 的 SMP 内核(ESP32-S3 是双核)里, portENTER_CRITICAL(lock)
 * 根本不看你传的锁 —— 它被展开成 vPortEnterCritical(), 只会关本核中断。
 * 而"临界区嵌套计数"和"保存的中断阈值"是两个核共用的全局量:
 * 两个核同时进出临界区就会把计数搞乱, 先退出的那个核会把全局的
 * 任务锁/ISR 锁提前释放掉, 后退出的那个核发现计数没归零,
 * 本核的中断阈值就再也恢复不了 —— 看门狗随即把板子复位。
 * 症状是: 板子每隔十几秒重启一次, 别的功能看着都正常。
 *
 * 所以这里统一换成 FreeRTOS 的递归互斥量:
 *   - 同一个核上的不同任务: 持锁期间不会被抢占, 天然互斥
 *   - 不同核之间: 由 SMP 内核自己的队列锁保证互斥
 * 临界区里只做 memcpy / 计数器自增, 不会阻塞, 所以拿锁不会卡住 UI。
 */
#ifndef DASH_LOCK_H_
#define DASH_LOCK_H_

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SemaphoreHandle_t handle;
    StaticSemaphore_t  buf;
} dash_lock_t;

/** 建锁。必须在任何 dash_lock_enter() 之前调一次(重复调无害)。 */
void dash_lock_init(dash_lock_t * lock);

/** 加锁/解锁。可重入: 同一个任务重复加锁不会把自己锁死。 */
void dash_lock_enter(dash_lock_t * lock);
void dash_lock_leave(dash_lock_t * lock);

#ifdef __cplusplus
}
#endif

#endif  // DASH_LOCK_H_
