// Host shim — ui_internal.h declares `extern SemaphoreHandle_t s_mtx;`. The
// pure helpers under test never touch it, so an opaque pointer type suffices.
#pragma once
typedef void *SemaphoreHandle_t;
