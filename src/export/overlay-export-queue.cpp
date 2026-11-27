#include "overlay-export-queue.h"

#include <obs-data.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "overlay-string-utils.h"
#include "overlay-trimmer.h"

namespace {

std::string ExtForFormat(RebuildOutputFormat fmt)
{
	switch (fmt) {
	case RebuildOutputFormat::Gif:
		return ".gif";
	case RebuildOutputFormat::Mov:
		return ".mov";
	case RebuildOutputFormat::Mp4:
	default:
		return ".mp4";
	}
}

} // namespace

OverlayExportQueue::~OverlayExportQueue()
{
	shuttingDown_ = true;
	CancelAll();
	std::vector<std::shared_ptr<ActiveWorker>> toJoin;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		toJoin = workers_;
	}
	for (auto &w : toJoin) {
		if (w && w->thread.joinable())
			w->thread.join();
	}
}

void OverlayExportQueue::SetCallbacks(QueueChangedCallback onChanged, JobCompletedCallback onJobDone)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onChanged_ = std::move(onChanged);
	onJobDone_ = std::move(onJobDone);
}

void OverlayExportQueue::SetMaxParallel(int maxParallel)
{
	std::lock_guard<std::mutex> lock(mutex_);
	maxParallel_ = std::clamp(maxParallel, 1, 4);
}

int OverlayExportQueue::MaxParallel() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return maxParallel_;
}

void OverlayExportQueue::SetQueueMode(ExportQueueMode mode)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		mode_ = mode;
	}
	SaveToDisk();
	if (onChanged_)
		onChanged_();
}

ExportQueueMode OverlayExportQueue::QueueMode() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return mode_;
}

void OverlayExportQueue::SetConfigPath(const std::string &jsonPath)
{
	std::lock_guard<std::mutex> lock(mutex_);
	persistPath_ = jsonPath;
}

void OverlayExportQueue::LoadFromDisk()
{
	std::string path;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		path = persistPath_;
	}
	if (path.empty())
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(path.c_str(), ".bak");
	if (!root)
		return;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		mode_ = static_cast<ExportQueueMode>(
			std::clamp<int>(static_cast<int>(obs_data_get_int(root, "mode")), 0, 1));
		if (obs_data_has_user_value(root, "max_parallel"))
			maxParallel_ = std::clamp<int>(static_cast<int>(obs_data_get_int(root, "max_parallel")), 1, 4);

		jobs_.clear();
		if (obs_data_array_t *arr = obs_data_get_array(root, "jobs")) {
			size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; ++i) {
				obs_data_t *obj = obs_data_array_item(arr, i);
				if (!obj)
					continue;
				ExportQueueJob job;
				job.id = obs_data_get_string(obj, "id");
				job.inputPath = obs_data_get_string(obj, "input");
				job.displayName = obs_data_get_string(obj, "name");
				job.startSeconds = obs_data_get_double(obj, "start");
				job.endSeconds = obs_data_get_double(obj, "end");
				job.markerColorRgb = static_cast<uint32_t>(obs_data_get_int(obj, "marker_color"));
				job.status = static_cast<ExportJobStatus>(obs_data_get_int(obj, "status"));
				if (job.status == ExportJobStatus::Running)
					job.status = ExportJobStatus::Queued;
				job.errorText = obs_data_get_string(obj, "error");
				job.outputPath = obs_data_get_string(obj, "output");
				job.outputFolder = obs_data_get_string(obj, "output_folder");
				if (obs_data_t *pr = obs_data_get_obj(obj, "preset")) {
					job.preset = DeserializeRebuildPreset(pr);
					obs_data_release(pr);
				}
				if (obs_data_array_t *tarr = obs_data_get_array(obj, "tracks")) {
					size_t tn = obs_data_array_count(tarr);
					for (size_t ti = 0; ti < tn; ++ti) {
						if (obs_data_t *tiObj = obs_data_array_item(tarr, ti)) {
							job.streamIndicesToKeep.push_back(
								static_cast<int>(obs_data_get_int(tiObj, "i")));
							obs_data_release(tiObj);
						}
					}
					obs_data_array_release(tarr);
				}
				if (!job.id.empty() && !job.inputPath.empty())
					jobs_.push_back(std::move(job));
				obs_data_release(obj);
			}
			obs_data_array_release(arr);
		}
	}
	obs_data_release(root);
	if (onChanged_)
		onChanged_();
}

void OverlayExportQueue::SaveToDisk() const
{
	std::string path;
	obs_data_t *root = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		path = persistPath_;
		obs_data_set_int(root, "mode", static_cast<long long>(mode_));
		obs_data_set_int(root, "max_parallel", maxParallel_);
		obs_data_array_t *arr = obs_data_array_create();
		for (const auto &job : jobs_) {
			obs_data_t *obj = obs_data_create();
			obs_data_set_string(obj, "id", job.id.c_str());
			obs_data_set_string(obj, "input", job.inputPath.c_str());
			obs_data_set_string(obj, "name", job.displayName.c_str());
			obs_data_set_double(obj, "start", job.startSeconds);
			obs_data_set_double(obj, "end", job.endSeconds);
			obs_data_set_int(obj, "marker_color", job.markerColorRgb);
			obs_data_set_int(obj, "status", static_cast<long long>(job.status));
			obs_data_set_string(obj, "error", job.errorText.c_str());
			obs_data_set_string(obj, "output", job.outputPath.c_str());
			obs_data_set_string(obj, "output_folder", job.outputFolder.c_str());
			obs_data_t *pr = SerializeRebuildPreset(job.preset);
			obs_data_set_obj(obj, "preset", pr);
			obs_data_release(pr);
			obs_data_array_t *tarr = obs_data_array_create();
			for (int sIdx : job.streamIndicesToKeep) {
				obs_data_t *ti = obs_data_create();
				obs_data_set_int(ti, "i", sIdx);
				obs_data_array_push_back(tarr, ti);
				obs_data_release(ti);
			}
			obs_data_set_array(obj, "tracks", tarr);
			obs_data_array_release(tarr);
			obs_data_array_push_back(arr, obj);
			obs_data_release(obj);
		}
		obs_data_set_array(root, "jobs", arr);
		obs_data_array_release(arr);
	}
	if (!path.empty())
		obs_data_save_json_safe(root, path.c_str(), ".tmp", ".bak");
	obs_data_release(root);
}

std::string OverlayExportQueue::MakeUniqueOutputPath(const std::string &folder, const std::string &stem,
						     RebuildOutputFormat fmt) const
{
	std::filesystem::path dir = folder.empty() ? std::filesystem::path(".") : std::filesystem::path(folder);
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::string ext = ExtForFormat(fmt);
	std::filesystem::path candidate = dir / (stem + "_share" + ext);
	for (int n = 1; n < 1000 && std::filesystem::exists(candidate, ec); ++n) {
		candidate = dir / (stem + "_share_" + std::to_string(n) + ext);
	}
	return candidate.string();
}

std::string OverlayExportQueue::EnqueueJob(const ExportQueueJob &tmpl, bool startImmediately)
{
	ExportQueueJob job = tmpl;
	auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			     std::chrono::system_clock::now().time_since_epoch())
			     .count();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		char idBuf[64];
		std::snprintf(idBuf, sizeof(idBuf), "job_%lld_%zu", static_cast<long long>(nowMs),
			      jobs_.size() + 1);
		job.id = idBuf;
		if (job.displayName.empty())
			job.displayName = overlay_str::FileNameFromPath(job.inputPath);
		if (job.outputFolder.empty())
			job.outputFolder = overlay_str::ParentDirFromPath(job.inputPath);
		if (job.outputPath.empty()) {
			std::string stem = std::filesystem::path(job.inputPath).stem().string();
			job.outputPath = MakeUniqueOutputPath(job.outputFolder, stem, job.preset.format);
		}
		job.markerColorRgb = kMarkerPalette[nextColorIdx_++ % 8];
		job.status = ExportJobStatus::Queued;
		job.progress = 0.0f;
		job.errorText.clear();
		jobs_.push_back(job);
	}

	SaveToDisk();
	if (onChanged_)
		onChanged_();
	if (startImmediately)
		StartQueue();
	return job.id;
}

void OverlayExportQueue::StartQueue()
{
	ScheduleMore();
}

void OverlayExportQueue::ScheduleMore()
{
	std::vector<std::function<void()>> launches;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (shuttingDown_)
			return;

		// Reap finished threads
		for (auto it = workers_.begin(); it != workers_.end();) {
			bool stillRunning = false;
			for (const auto &j : jobs_) {
				if (j.id == (*it)->id && j.status == ExportJobStatus::Running) {
					stillRunning = true;
					break;
				}
			}
			if (!stillRunning && mode_ == ExportQueueMode::Separate) {
				if ((*it)->thread.joinable())
					(*it)->thread.detach();
				it = workers_.erase(it);
			} else {
				++it;
			}
		}

		if (mode_ == ExportQueueMode::Merge) {
			bool anyRunning = std::any_of(jobs_.begin(), jobs_.end(), [](const ExportQueueJob &j) {
				return j.status == ExportJobStatus::Running;
			});
			if (!anyRunning) {
				std::vector<ExportQueueJob> toMerge;
				for (auto &j : jobs_) {
					if (j.status == ExportJobStatus::Queued) {
						j.status = ExportJobStatus::Running;
						j.progress = 0.0f;
						j.errorText.clear();
						toMerge.push_back(j);
					}
				}
				if (!toMerge.empty()) {
					auto w = std::make_shared<ActiveWorker>();
					w->id = "merge";
					workers_.push_back(w);
					launches.push_back([this, toMerge, w]() {
						w->thread = std::thread(&OverlayExportQueue::RunMergeWorker,
									this, toMerge, w);
					});
				}
			}
		} else {
			int running = 0;
			for (const auto &j : jobs_) {
				if (j.status == ExportJobStatus::Running)
					++running;
			}
			for (auto &j : jobs_) {
				if (running >= maxParallel_)
					break;
				if (j.status == ExportJobStatus::Queued) {
					j.status = ExportJobStatus::Running;
					j.progress = 0.0f;
					j.errorText.clear();
					++running;
					auto w = std::make_shared<ActiveWorker>();
					w->id = j.id;
					workers_.push_back(w);
					ExportQueueJob copyJob = j;
					launches.push_back([this, copyJob, w]() {
						w->thread = std::thread(&OverlayExportQueue::RunSeparateWorker,
									this, copyJob, w);
					});
				}
			}
		}
	}

	for (auto &fn : launches)
		fn();
	if (onChanged_)
		onChanged_();
}

void OverlayExportQueue::RunSeparateWorker(ExportQueueJob job, std::shared_ptr<ActiveWorker> worker)
{
	OverlayExportSegment seg;
	seg.inputPath = job.inputPath;
	seg.startSeconds = job.startSeconds;
	seg.endSeconds = job.endSeconds;
	seg.streamIndicesToKeep = job.streamIndicesToKeep;
	seg.options = job.preset.ToExportOptions();

	std::string err;
	bool ok = OverlayTrimmer::ExportSegment(
		seg, job.outputPath, &worker->cancel,
		[&](float p) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				for (auto &j : jobs_) {
					if (j.id == job.id) {
						j.progress = p;
						break;
					}
				}
			}
			if (onChanged_)
				onChanged_();
		},
		err);

	ExportQueueJob finishedJob = job;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &j : jobs_) {
			if (j.id == job.id) {
				j.status = ok ? ExportJobStatus::Done
					      : (worker->cancel.load() ? ExportJobStatus::Cancelled
								       : ExportJobStatus::Failed);
				j.progress = ok ? 1.0f : j.progress;
				j.errorText = ok ? std::string{} : err;
				finishedJob = j;
				break;
			}
		}
	}

	SaveToDisk();
	if (onJobDone_)
		onJobDone_(finishedJob, false, {});
	if (onChanged_)
		onChanged_();
	ScheduleMore();
}

void OverlayExportQueue::RunMergeWorker(std::vector<ExportQueueJob> jobs,
					std::shared_ptr<ActiveWorker> worker)
{
	if (jobs.empty())
		return;

	std::time_t now = std::time(nullptr);
	std::tm tmNow{};
	localtime_r(&now, &tmNow);
	char tsBuf[48];
	std::snprintf(tsBuf, sizeof(tsBuf), "merge_%04u%02u%02u_%02u%02u%02u",
		      static_cast<unsigned>(tmNow.tm_year + 1900), static_cast<unsigned>(tmNow.tm_mon + 1),
		      static_cast<unsigned>(tmNow.tm_mday), static_cast<unsigned>(tmNow.tm_hour),
		      static_cast<unsigned>(tmNow.tm_min), static_cast<unsigned>(tmNow.tm_sec));

	std::string folder = jobs[0].outputFolder;
	std::string outPath = (std::filesystem::path(folder.empty() ? "." : folder) /
			       (std::string(tsBuf) + ExtForFormat(jobs[0].preset.format)))
				      .string();

	std::vector<OverlayExportSegment> segs;
	segs.reserve(jobs.size());
	for (const auto &j : jobs) {
		OverlayExportSegment s;
		s.inputPath = j.inputPath;
		s.startSeconds = j.startSeconds;
		s.endSeconds = j.endSeconds;
		s.streamIndicesToKeep = j.streamIndicesToKeep;
		s.options = j.preset.ToExportOptions();
		segs.push_back(std::move(s));
	}

	std::string err;
	bool ok = OverlayTrimmer::ExportSegmentsMerged(
		segs, outPath, jobs[0].preset.ToExportOptions(), &worker->cancel,
		[&](float p) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				for (const auto &mj : jobs) {
					for (auto &j : jobs_) {
						if (j.id == mj.id) {
							j.progress = p;
							break;
						}
					}
				}
			}
			if (onChanged_)
				onChanged_();
		},
		err);

	ExportQueueJob repJob = jobs[0];
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &mj : jobs) {
			for (auto &j : jobs_) {
				if (j.id == mj.id) {
					j.status = ok ? ExportJobStatus::Done
						      : (worker->cancel.load() ? ExportJobStatus::Cancelled
									       : ExportJobStatus::Failed);
					j.progress = ok ? 1.0f : j.progress;
					j.errorText = ok ? std::string{} : err;
					if (ok)
						j.outputPath = outPath;
					repJob = j;
					break;
				}
			}
		}
		for (auto it = workers_.begin(); it != workers_.end();) {
			if ((*it).get() == worker.get()) {
				if ((*it)->thread.joinable())
					(*it)->thread.detach();
				it = workers_.erase(it);
			} else {
				++it;
			}
		}
	}

	SaveToDisk();
	if (onJobDone_)
		onJobDone_(repJob, true, ok ? outPath : std::string{});
	if (onChanged_)
		onChanged_();
}

void OverlayExportQueue::CancelAll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &w : workers_) {
		if (w)
			w->cancel = true;
	}
	for (auto &j : jobs_) {
		if (j.status == ExportJobStatus::Queued)
			j.status = ExportJobStatus::Cancelled;
	}
}

void OverlayExportQueue::RetryJob(const std::string &jobId)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &j : jobs_) {
			if (j.id == jobId &&
			    (j.status == ExportJobStatus::Failed || j.status == ExportJobStatus::Cancelled)) {
				j.status = ExportJobStatus::Queued;
				j.progress = 0.0f;
				j.errorText.clear();
				break;
			}
		}
	}
	SaveToDisk();
	ScheduleMore();
}

void OverlayExportQueue::RemoveJob(const std::string &jobId)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &w : workers_) {
			if (w && w->id == jobId)
				w->cancel = true;
		}
		jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
					   [&](const ExportQueueJob &j) {
						   return j.id == jobId && j.status != ExportJobStatus::Running;
					   }),
			    jobs_.end());
	}
	SaveToDisk();
	if (onChanged_)
		onChanged_();
}

void OverlayExportQueue::ClearFinished()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
					   [](const ExportQueueJob &j) {
						   return j.status == ExportJobStatus::Done ||
							  j.status == ExportJobStatus::Failed ||
							  j.status == ExportJobStatus::Cancelled;
					   }),
			    jobs_.end());
	}
	SaveToDisk();
	if (onChanged_)
		onChanged_();
}

void OverlayExportQueue::ClearAll()
{
	CancelAll();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
					   [](const ExportQueueJob &j) {
						   return j.status != ExportJobStatus::Running;
					   }),
			    jobs_.end());
	}
	SaveToDisk();
	if (onChanged_)
		onChanged_();
}

void OverlayExportQueue::UpdateJobTrim(const std::string &jobId, double startSec, double endSec,
				       const std::string &newOutputPath)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &j : jobs_) {
			if (j.id == jobId) {
				j.startSeconds = startSec;
				j.endSeconds = endSec;
				if (!newOutputPath.empty())
					j.outputPath = newOutputPath;
				break;
			}
		}
	}
	SaveToDisk();
	if (onChanged_)
		onChanged_();
}

std::vector<ExportQueueJob> OverlayExportQueue::SnapshotJobs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return jobs_;
}

bool OverlayExportQueue::HasActiveJobs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::any_of(jobs_.begin(), jobs_.end(), [](const ExportQueueJob &j) {
		return j.status == ExportJobStatus::Running || j.status == ExportJobStatus::Queued;
	});
}

int OverlayExportQueue::ActiveCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int c = 0;
	for (const auto &j : jobs_) {
		if (j.status == ExportJobStatus::Running || j.status == ExportJobStatus::Queued)
			++c;
	}
	return c;
}

int OverlayExportQueue::TotalCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return static_cast<int>(jobs_.size());
}
