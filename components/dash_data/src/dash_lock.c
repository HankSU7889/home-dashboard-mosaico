/**
 * @file dash_lock.c
 * 看板共享状态的跨核互斥锁(实现)
 */

#include "dash_lock.h"

#include "esp_log.h"

static const char * TAG = "dash_lock";

void dash_lock_init(dash_lock_t * lock)
{
    if(lock == NULL) return;
    if(lock->handle != NULL) return;
    lock->handle = xSemaphoreCreateRecursiveMutexStatic(&lock->buf);
    if(lock->handle == NULL) {
        ESP_LOGE(TAG, "互斥量创建失败");
    }
}

void dash_lock_enter(dash_lock_t * lock)
{
    /* 没建好就当没锁: 顶多读到半新半旧的数据, 总比直接崩了强 */
    if(lock == NULL || lock->handle == NULL) return;
    xSemaphoreTakeRecursive(lock->handle, portMAX_DELAY);
}

void dash_lock_leave(dash_lock_t * lock)
{
    if(lock == NULL || lock->handle == NULL) return;
    xSemaphoreGiveRecursive(lock->handle);
}
