#pragma once

#include <obs-data.h>
#include <string>
#include <vector>

#include "overlay-export-options.h"

struct RebuildPreset {
	std::string id;
	std::string name;
	RebuildMode mode = RebuildMode::Reencode;
	RebuildOutputFormat format = RebuildOutputFormat::Mp4;
	int crf = 23;
	RebuildQuality quality = RebuildQuality::Medium;
	int width = 0;
	int height = 0;
	double fps = 0.0;
	int gifWidth = 480;
	int gifHeight = 0;
	int gifFps = 15;
	int gifColors = 128;
	int gifDither = 1;
	bool gifLoop = true;
	bool keepAudio = true;
	bool keepVideo = true;
	bool tracksConfigured = false;
	std::vector<int> keepAudioOrdinals;

	OverlayExportOptions ToExportOptions() const;
	std::string ResolutionLabel() const;
	std::string FormatLabel() const;
};

class RebuildPresetStore {
public:
	void LoadFromSettings(obs_data_t *settings);
	void SaveToSettings(obs_data_t *settings) const;
	void SeedDefaultsIfNeeded(obs_data_t *settings);

	const std::vector<RebuildPreset> &Presets() const { return presets_; }
	const std::string &SelectedId() const { return selectedId_; }
	void SetSelectedId(const std::string &id);
	const RebuildPreset *SelectedPreset() const;
	const RebuildPreset *FindById(const std::string &id) const;

	RebuildPreset CreateCustomPreset(const std::string &baseName, const RebuildPreset &tmpl);
	bool UpdatePreset(const RebuildPreset &preset);
	bool DeletePreset(const std::string &id);

private:
	std::vector<RebuildPreset> presets_;
	std::string selectedId_ = "seed_balanced";
};

obs_data_t *SerializeRebuildPreset(const RebuildPreset &p);
RebuildPreset DeserializeRebuildPreset(obs_data_t *obj);
