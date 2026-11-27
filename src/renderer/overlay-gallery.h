#pragma once

#include <QImage>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GalleryItem {
	std::string filePath;
	std::string fileName;
	std::string extension;
	uint64_t sizeBytes = 0;
	double durationSec = 0.0;
	int64_t mtimeEpochSec = 0;
	QImage thumbnail;
	bool thumbRequested = false;
	bool thumbLoaded = false;
};

class OverlayGallery {
public:
	explicit OverlayGallery();
	~OverlayGallery();

	void Refresh(const std::string &directory, std::function<void()> onThumbReady);
	void StopWorkers();

	std::vector<GalleryItem> SnapshotItems() const;
	bool GetThumbnail(const std::string &filePath, QImage &outThumb) const;
	size_t ItemCount() const;
	const std::string &CurrentDirectory() const { return currentDir_; }

	static bool OpenInFileManager(const std::string &filePath);
	static bool OpenWithDefaultApp(const std::string &filePath);
	static void CopyPathToClipboard(const std::string &filePath);
	static void CopyFileToClipboard(const std::string &filePath);
	static bool RenameMediaFile(const std::string &oldPath, const std::string &newStem,
				    std::string &outNewPath, std::string &outError);
	static bool DeleteMediaFile(const std::string &filePath);

private:
	void WorkerLoop();
	void StartWorkerIfNeeded();

	mutable std::mutex mutex_;
	std::vector<GalleryItem> items_;
	std::deque<std::string> thumbQueue_;
	std::condition_variable cv_;
	std::thread worker_;
	std::atomic<bool> stop_{false};
	bool workerRunning_ = false;
	std::string currentDir_;
	std::function<void()> onThumbReady_;
};
