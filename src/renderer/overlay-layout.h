#pragma once

#include <QRect>
#include <array>

#include "win-d2d-compat.h"

enum class OverlayPosition {
	Top = 0,
	Bottom = 1,
	Left = 2,
	Right = 3,
	TopLeft = 4,
	TopRight = 5,
	BottomLeft = 6,
	BottomRight = 7,
	Center = 8,
};

enum class OverlayOrientation {
	Horizontal = 0,
	Vertical = 1,
};

enum class OverlayMonitorMode {
	Primary = 0,
	Specific = 1,
	Cursor = 2,
};

struct OverlayLayout {
	float windowWidth = 0.0f;
	float windowHeight = 0.0f;
	float scale = 1.0f;

	// Monitor-local origin & bounding box of the compact HUD anchor
	overlay_compat::RectF hudAnchorBounds{};
	overlay_compat::RectF mainPanel{};
	overlay_compat::RectF statsPanel{};
	overlay_compat::RectF clipPanel{};

	overlay_compat::RectF toggleObsBtn{};
	overlay_compat::RectF recordBtn{};
	overlay_compat::RectF replayToggleBtn{};
	overlay_compat::RectF replaySaveBtn{};
	overlay_compat::RectF galleryBtn{};
	overlay_compat::RectF settingsBtn{};
	overlay_compat::RectF statsToggleBtn{};

	std::array<overlay_compat::RectF, 4> clipSlotBtns{};
	int visibleClipCount = 0;

	static constexpr float kHorizBaseWidth = 490.0f;
	static constexpr float kHorizBaseHeight = 140.0f;
	static constexpr float kHorizWindowWidth = 578.0f;

	static constexpr float kVertBaseWidth = 180.0f;
	static constexpr float kVertBaseHeight = 390.0f;
	static constexpr float kVertWindowWidth = 268.0f;

	static constexpr float kStatsHorizWidth = 200.0f;
	static constexpr float kStatsVertWidth = 160.0f;
	static constexpr float kStatsHeight = 102.0f;
};

class OverlayLayoutManager {
public:
	static void CalculateLayout(OverlayPosition pos, int margin, OverlayOrientation orientation,
				    bool statsVisible, int scalePercent, float monitorWidth,
				    float monitorHeight, int visibleClipSlots, OverlayLayout &out);
};
