#include "plugin-support.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void obs_log(int log_level, const char *format, ...)
{
	if (!format)
		return;

	size_t len = strlen(format) + strlen(PLUGIN_NAME) + 8;
	char *tmpl = (char *)malloc(len);
	if (!tmpl)
		return;

	snprintf(tmpl, len, "[%s] %s", PLUGIN_NAME, format);

	va_list args;
	va_start(args, format);
	blogva(log_level, tmpl, args);
	va_end(args);

	free(tmpl);
}
