#ifndef FALL_DETECTION_MAIN_AI_INFERENCE_LOCK_H_
#define FALL_DETECTION_MAIN_AI_INFERENCE_LOCK_H_

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void InitializeAiInferenceLock();
bool LockAiInference(TickType_t timeout_ticks);
void UnlockAiInference();

#endif
