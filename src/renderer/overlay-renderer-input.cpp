#include "overlay-renderer.h"
#include "overlay-indicators-c.h"

#include <algorithm>

void OverlayRenderer::TouchInteractionClock()
{
	lastInteractionMs_ = overlay_compat::GetTickCount64Ms();
}

void OverlayRenderer::HandleMouseMove(float x, float y)
{
	mouseX_ = x;
	mouseY_ = y;

	if (!activeDragId_.empty()) {
		for (const auto &z : hitZones_) {
			if (z.id == activeDragId_ && z.onDrag) {
				TouchInteractionClock();
				z.onDrag(x, y);
				winMgr_.RequestRepaint();
				return;
			}
		}
	}

	std::string newHover;
	for (auto it = hitZones_.rbegin(); it != hitZones_.rend(); ++it) {
		if (it->rect.Contains(x, y) && (it->onPress || it->onDrag)) {
			newHover = it->id;
			break;
		}
	}

	if (newHover != hoveredId_) {
		hoveredId_ = std::move(newHover);
		if (!hoveredId_.empty())
			TouchInteractionClock();
		winMgr_.RequestRepaint();
	}
}

void OverlayRenderer::HandleMouseButton(float x, float y, Qt::MouseButton btn, bool pressed)
{
	TouchInteractionClock();
	mouseX_ = x;
	mouseY_ = y;

	if (!pressed) {
		if (!activeDragId_.empty()) {
			for (const auto &z : hitZones_) {
				if (z.id == activeDragId_ && z.onRelease)
					z.onRelease();
			}
			activeDragId_.clear();
			winMgr_.RequestRepaint();
		}
		return;
	}

	for (auto it = hitZones_.rbegin(); it != hitZones_.rend(); ++it) {
		if (it->rect.Contains(x, y)) {
			if (it->onDrag)
				activeDragId_ = it->id;
			if (it->onPress)
				it->onPress(x, y, btn);
			winMgr_.RequestRepaint();
			return;
		}
	}
}

void OverlayRenderer::HandleWheel(float x, float y, int deltaY)
{
	TouchInteractionClock();
	for (auto it = hitZones_.rbegin(); it != hitZones_.rend(); ++it) {
		if (it->rect.Contains(x, y) && it->onWheel) {
			it->onWheel(deltaY);
			return;
		}
	}
}

void OverlayRenderer::HandleKey(int qtKey, uint32_t /*mods*/, const QString & /*text*/, bool pressed)
{
	if (!pressed)
		return;
	TouchInteractionClock();

	if (qtKey == Qt::Key_Escape) {
		if (viewMode_ != ViewMode::HudOnly) {
			viewMode_ = ViewMode::HudOnly;
			cfg_.indicatorsPreview = false;
			overlay_indicators_set_preview(false);
			winMgr_.SetKeyboardInteractive(false);
			winMgr_.RequestRepaint();
		} else {
			Hide();
		}
		return;
	}

	if (viewMode_ == ViewMode::Gallery && player_.IsOpen()) {
		switch (qtKey) {
		case Qt::Key_Space:
			player_.TogglePlayPause();
			winMgr_.RequestRepaint();
			break;
		case Qt::Key_Right:
			player_.Seek(player_.PositionSeconds() + 5.0);
			break;
		case Qt::Key_Left:
			player_.Seek(std::max(0.0, player_.PositionSeconds() - 5.0));
			break;
		case Qt::Key_Comma:
			player_.StepFrame(-1);
			break;
		case Qt::Key_Period:
			player_.StepFrame(1);
			break;
		case Qt::Key_I:
			trimStartSec_ = player_.PositionSeconds();
			winMgr_.RequestRepaint();
			break;
		case Qt::Key_O:
			trimEndSec_ = std::max(trimStartSec_, player_.PositionSeconds());
			winMgr_.RequestRepaint();
			break;
		default:
			break;
		}
	}
}
