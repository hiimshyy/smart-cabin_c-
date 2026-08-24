// Local override: mute per-frame awnn_lib verbose logs.
// awnn_lib.c uses `#include <log/log.h>` — with `-I.` before `-I$(AI_SDK)` in
// Makefile, this file shadows the SDK's noisy log/log.h.
#pragma once
#include <stdio.h>

#define ALOGD(...) ((void)0)
#define ALOGV(...) ((void)0)
#define ALOGI(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGE(fmt, ...) fprintf(stderr, "[awnn:E] " fmt, ##__VA_ARGS__)

#define SLOGD(...) ((void)0)
#define SLOGV(...) ((void)0)
#define SLOGI(...) ((void)0)
#define SLOGW(...) ((void)0)
#define SLOGE(fmt, ...) fprintf(stderr, "[awnn:E] " fmt, ##__VA_ARGS__)

#define CONDITION(cond) (__builtin_expect((cond)!=0, 0))
#define LOG_ALWAYS_FATAL_IF(cond, ...) ((void)0)
#define LOG_FATAL_IF(cond, ...) ((void)0)
#define ALOG_ASSERT(cond, ...) ((void)0)
#define LOG_ALWAYS_FATAL(...) ((void)0)
#define ALOGW_IF(cond, ...) ((void)0)
#define LOG_EVENT_INT(_tag, _value)
