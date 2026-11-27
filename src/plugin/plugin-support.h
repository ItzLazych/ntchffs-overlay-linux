#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <util/base.h>

#ifndef PLUGIN_NAME
#define PLUGIN_NAME "ntchffs-overlay"
#endif

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "1.0.0"
#endif

void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
