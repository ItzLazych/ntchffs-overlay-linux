#pragma once

#include <QImage>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioStreamInfo {
	int streamIndex = -1;
	int audioOrdinal = 0;
	std::string name;
	std::string language;
	int channels = 2;
	int sampleRate = 48000;
};

class OverlayVideoPlayer {
public:
	OverlayVideoPlayer() = default;
	~OverlayVideoPlayer();

	bool Open(const std::string &filePath, std::function<void()> onFrameReady);
	void Close();

	bool IsOpen() const { return open_.load(); }
	const std::string &FilePath() const { return filePath_; }

	void Play();
	void Pause();
	void TogglePlayPause();
	bool IsPlaying() const { return playing_.load(); }

	void Seek(double seconds);
	void StepFrame(int deltaFrames);

	double DurationSeconds() const { return durationSec_; }
	double PositionSeconds() const { return positionSec_.load(); }
	int VideoWidth() const { return videoWidth_; }
	int VideoHeight() const { return videoHeight_; }
	double VideoFps() const { return videoFps_; }

	void SetVolume(float volume01);
	float Volume() const { return volume_.load(); }
	void SetMuted(bool muted);
	bool IsMuted() const { return muted_.load(); }

	const std::vector<AudioStreamInfo> &AudioStreams() const { return audioStreams_; }
	void SelectAudioStream(int streamIndex);
	int ActiveAudioStreamIndex() const { return activeAudioStream_.load(); }

	// Returns latest decoded frame into dst (thread-safe).
	bool CopyCurrentFrame(QImage &dst, int *outW = nullptr, int *outH = nullptr) const;

private:
	void PlayerLoop();

	std::string filePath_;
	std::function<void()> onFrameReady_;

	std::atomic<bool> open_{false};
	std::atomic<bool> stopRequested_{false};
	std::atomic<bool> playing_{false};
	std::atomic<bool> seekPending_{false};
	std::atomic<double> seekTargetSec_{0.0};
	std::atomic<int> stepDelta_{0};
	std::atomic<double> positionSec_{0.0};
	std::atomic<float> volume_{0.8f};
	std::atomic<bool> muted_{false};
	std::atomic<int> activeAudioStream_{-1};

	double durationSec_ = 0.0;
	double videoFps_ = 60.0;
	int videoWidth_ = 0;
	int videoHeight_ = 0;
	int videoStreamIndex_ = -1;
	std::vector<AudioStreamInfo> audioStreams_;

	mutable std::mutex frameMutex_;
	QImage currentFrame_;

	std::thread worker_;
};
