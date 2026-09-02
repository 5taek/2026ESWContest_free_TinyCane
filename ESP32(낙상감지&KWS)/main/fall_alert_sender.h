#ifndef FALL_DETECTION_MAIN_FALL_ALERT_SENDER_H_
#define FALL_DETECTION_MAIN_FALL_ALERT_SENDER_H_

void InitializeFallAlertSender();
bool QueueFallState(const char* state, const char* reason, float score);

#endif
