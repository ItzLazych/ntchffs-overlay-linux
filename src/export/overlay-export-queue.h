#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "overlay-rebuild-preset.h"

enum class ExportQueueMode {
	Separate = 0,
	Merge = 1,
};

enum class ExportJobStatus {
	Queued = 0,
	Running = 1,
	Done = 2,
	Failed = 3,
	Cancelled = 4,
};

struct ExportQueueJob {
	std::string id;
	std::string inputPath;
	std::string displayName;
	double startSeconds = 0.0;
	double endSeconds = 0.0;
	RebuildPreset preset;
	std::vector<int> streamIndicesToKeep;
	std::string outputPath;
	std::string outputFolder;
	ExportJobStatus status = ExportJobStatus::Queued;
	std::string errorText;
	float progress = 0.0f;
	uint32_t markerColorRgb = 0xe67e22;
};

class OverlayExportQueue {
public:
	static constexpr uint32_t kMarkerPalette[8] = {
		0xe67e22, 0x3498db, 0x2ecc71, 0x9b59b6, 0xe74c3c, 0x1abc9c, 0xf1c40f, 0xe84393,
	};

	using QueueChangedCallback = std::function<void()>;
	using JobCompletedCallback = std::function<void(const ExportQueueJob &job, bool merged, const std::string &mergedPath)>;

	OverlayExportQueue() = default;
	~OverlayExportQueue();

	void SetCallbacks(QueueChangedCallback onChanged, JobCompletedCallback onJobDone);
	void SetMaxParallel(int maxParallel);
	int MaxParallel() const;
	void SetQueueMode(ExportQueueMode mode);
	ExportQueueMode QueueMode() const;

	void SetConfigPath(const std::string &jsonPath);
	void LoadFromDisk();
	void SaveToDisk() const;

	std::string EnqueueJob(const ExportQueueJob &tmpl, bool startImmediately = false);
	void StartQueue();
	void CancelAll();
	void RetryJob(const std::string &jobId);
	void RemoveJob(const std::string &jobId);
	void ClearFinished();
	void ClearAll();

	void UpdateJobTrim(const std::string &jobId, double startSec, double endSec, const std::string &newOutputPath);
	std::vector<ExportQueueJob> SnapshotJobs() const;
	bool HasActiveJobs() const;
	int ActiveCount() const;
	int TotalCount() const;

private:
	struct ActiveWorker {
		std::string id;
		std::atomic<bool> cancel{false};
		std::thread thread;
	};

	void ScheduleMore();
	void RunSeparateWorker(ExportQueueJob job, std::shared_ptr<ActiveWorker> worker);
	void RunMergeWorker(std::vector<ExportQueueJob> jobs, std::shared_ptr<ActiveWorker> worker);
	std::string MakeUniqueOutputPath(const std::string &folder, const std::string &stem,
					 RebuildOutputFormat fmt) const;

	mutable std::mutex mutex_;
	std::vector<ExportQueueJob> jobs_;
	std::vector<std::shared_ptr<ActiveWorker>> workers_;
	ExportQueueMode mode_ = ExportQueueMode::Separate;
	int maxParallel_ = 2;
	uint64_t nextColorIdx_ = 0;
	std::string persistPath_;
	std::atomic<bool> shuttingDown_{false};

	QueueChangedCallback onChanged_;
	JobCompletedCallback onJobDone_;
};
