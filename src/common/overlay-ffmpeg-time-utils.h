#pragma once

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

#include <cmath>
#include <cstdint>

namespace overlay::ffmpeg {

inline double PtsToSeconds(int64_t pts, AVRational tb)
{
	if (pts == AV_NOPTS_VALUE || tb.den == 0)
		return 0.0;
	return static_cast<double>(pts) * av_q2d(tb);
}

inline int64_t SecondsToPts(double seconds, AVRational tb)
{
	if (!std::isfinite(seconds) || tb.num == 0 || tb.den == 0)
		return 0;
	return static_cast<int64_t>(std::llround(seconds / av_q2d(tb)));
}

inline double ClampTime(double sec, double minSec, double maxSec)
{
	if (!std::isfinite(sec))
		return minSec;
	if (sec < minSec)
		return minSec;
	if (maxSec > minSec && sec > maxSec)
		return maxSec;
	return sec;
}

} // namespace overlay::ffmpeg
