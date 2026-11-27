#include "overlay-layout.h"

#include <algorithm>
#include <cmath>

void OverlayLayoutManager::CalculateLayout(OverlayPosition pos, int margin, OverlayOrientation orientation,
					   bool statsVisible, int scalePercent, float monitorWidth,
					   float monitorHeight, int visibleClipSlots, OverlayLayout &out)
{
	out = OverlayLayout{};
	const float s = std::clamp(static_cast<float>(scalePercent) / 100.0f, 0.5f, 2.5f);
	out.scale = s;
	out.windowWidth = monitorWidth;
	out.windowHeight = monitorHeight;
	out.visibleClipCount = std::clamp(visibleClipSlots, 0, 4);

	const float pad = 12.0f * s;
	const float gap = 8.0f * s;
	const float m = static_cast<float>(std::max(0, margin));

	float panelW = 0.0f;
	float panelH = 0.0f;
	if (orientation == OverlayOrientation::Horizontal) {
		panelW = OverlayLayout::kHorizBaseWidth * s;
		panelH = 68.0f * s;
	} else {
		panelW = 146.0f * s;
		panelH = OverlayLayout::kVertBaseHeight * s;
	}

	const float statsW = (orientation == OverlayOrientation::Vertical ? OverlayLayout::kStatsVertWidth
									  : OverlayLayout::kStatsHorizWidth) *
			     s;
	const float statsH = OverlayLayout::kStatsHeight * s;
	const float clipBarW = out.visibleClipCount > 0 ? (out.visibleClipCount * 52.0f + 16.0f) * s : 0.0f;
	const float clipBarH = 38.0f * s;

	// Total compact group dimensions for positioning
	float groupW = panelW;
	float groupH = panelH;
	if (orientation == OverlayOrientation::Horizontal) {
		if (statsVisible)
			groupH += gap + std::min(statsH * 0.58f, 52.0f * s);
		if (out.visibleClipCount > 0)
			groupH += gap + clipBarH;
	} else {
		if (statsVisible)
			groupW += gap + statsW;
	}

	float gx = m;
	float gy = m;

	switch (pos) {
	case OverlayPosition::Top:
		gx = (monitorWidth - groupW) * 0.5f;
		gy = m;
		break;
	case OverlayPosition::Bottom:
		gx = (monitorWidth - groupW) * 0.5f;
		gy = monitorHeight - groupH - m;
		break;
	case OverlayPosition::Left:
		gx = m;
		gy = (monitorHeight - groupH) * 0.5f;
		break;
	case OverlayPosition::Right:
		gx = monitorWidth - groupW - m;
		gy = (monitorHeight - groupH) * 0.5f;
		break;
	case OverlayPosition::TopLeft:
		gx = m;
		gy = m;
		break;
	case OverlayPosition::TopRight:
		gx = monitorWidth - groupW - m;
		gy = m;
		break;
	case OverlayPosition::BottomLeft:
		gx = m;
		gy = monitorHeight - groupH - m;
		break;
	case OverlayPosition::BottomRight:
		gx = monitorWidth - groupW - m;
		gy = monitorHeight - groupH - m;
		break;
	case OverlayPosition::Center:
		gx = (monitorWidth - groupW) * 0.5f;
		gy = (monitorHeight - groupH) * 0.5f;
		break;
	}

	gx = std::clamp(gx, 4.0f, std::max(4.0f, monitorWidth - groupW - 4.0f));
	gy = std::clamp(gy, 4.0f, std::max(4.0f, monitorHeight - groupH - 4.0f));

	const bool anchorBottom = (pos == OverlayPosition::Bottom || pos == OverlayPosition::BottomLeft ||
				   pos == OverlayPosition::BottomRight);
	const bool anchorRight = (pos == OverlayPosition::Right || pos == OverlayPosition::TopRight ||
				  pos == OverlayPosition::BottomRight);

	if (orientation == OverlayOrientation::Horizontal) {
		float mainY = gy;
		if (anchorBottom)
			mainY = gy + groupH - panelH;
		out.mainPanel = overlay_compat::MakeRectXYWH(gx, mainY, panelW, panelH);

		// 6 primary buttons + small stats chevron pill
		const float innerH = panelH - 16.0f * s;
		const float btnW = (panelW - pad * 2.0f - gap * 5.0f - 28.0f * s) / 5.0f;
		float bx = gx + pad;
		const float by = mainY + (panelH - innerH) * 0.5f;

		out.toggleObsBtn = overlay_compat::MakeRectXYWH(bx, by, btnW, innerH);
		bx += btnW + gap;
		out.recordBtn = overlay_compat::MakeRectXYWH(bx, by, btnW, innerH);
		bx += btnW + gap;
		out.replayToggleBtn = overlay_compat::MakeRectXYWH(bx, by, btnW, innerH);
		bx += btnW + gap;
		out.replaySaveBtn = overlay_compat::MakeRectXYWH(bx, by, btnW, innerH);
		bx += btnW + gap;
		out.galleryBtn = overlay_compat::MakeRectXYWH(bx, by, btnW, innerH);
		bx += btnW + gap;

		const float colW = 28.0f * s;
		const float halfH = (innerH - 4.0f * s) * 0.5f;
		out.settingsBtn = overlay_compat::MakeRectXYWH(bx, by, colW, halfH);
		out.statsToggleBtn = overlay_compat::MakeRectXYWH(bx, by + halfH + 4.0f * s, colW, halfH);

		float curBelowY = anchorBottom ? (mainY - gap) : (mainY + panelH + gap);
		if (statsVisible) {
			const float sh = 46.0f * s;
			const float sw = panelW;
			float sy = anchorBottom ? (curBelowY - sh) : curBelowY;
			out.statsPanel = overlay_compat::MakeRectXYWH(gx, sy, sw, sh);
			curBelowY = anchorBottom ? (sy - gap) : (sy + sh + gap);
		}

		if (out.visibleClipCount > 0) {
			const float cw = std::min(panelW, clipBarW);
			const float cx = gx + (panelW - cw) * 0.5f;
			float cy = anchorBottom ? (curBelowY - clipBarH) : curBelowY;
			out.clipPanel = overlay_compat::MakeRectXYWH(cx, cy, cw, clipBarH);
			const float sbGap = 6.0f * s;
			const float sbW = (cw - 16.0f * s - sbGap * std::max(0, out.visibleClipCount - 1)) /
					  std::max(1, out.visibleClipCount);
			for (int i = 0; i < out.visibleClipCount; ++i) {
				out.clipSlotBtns[i] = overlay_compat::MakeRectXYWH(
					cx + 8.0f * s + i * (sbW + sbGap), cy + 5.0f * s, sbW, clipBarH - 10.0f * s);
			}
		}
	} else {
		float mainX = gx;
		if (anchorRight && statsVisible)
			mainX = gx + statsW + gap;
		out.mainPanel = overlay_compat::MakeRectXYWH(mainX, gy, panelW, panelH);

		const float bw = panelW - 18.0f * s;
		const float rowCount = out.visibleClipCount > 0 ? 7.0f : 6.0f;
		const float bh = std::min(48.0f * s, (panelH - pad * 2.0f - gap * rowCount) / rowCount);
		float by = gy + pad;
		const float bx = mainX + 9.0f * s;

		out.toggleObsBtn = overlay_compat::MakeRectXYWH(bx, by, bw, bh);
		by += bh + gap;
		out.recordBtn = overlay_compat::MakeRectXYWH(bx, by, bw, bh);
		by += bh + gap;
		out.replayToggleBtn = overlay_compat::MakeRectXYWH(bx, by, bw, bh);
		by += bh + gap;
		out.replaySaveBtn = overlay_compat::MakeRectXYWH(bx, by, bw, bh);
		by += bh + gap;
		out.galleryBtn = overlay_compat::MakeRectXYWH(bx, by, bw, bh);
		by += bh + gap;

		const float halfW = (bw - 6.0f * s) * 0.5f;
		out.settingsBtn = overlay_compat::MakeRectXYWH(bx, by, halfW, 34.0f * s);
		out.statsToggleBtn = overlay_compat::MakeRectXYWH(bx + halfW + 6.0f * s, by, halfW, 34.0f * s);
		by += 34.0f * s + gap;

		if (out.visibleClipCount > 0) {
			const float clipH = std::max(28.0f * s, gy + panelH - pad - by);
			out.clipPanel = overlay_compat::MakeRectXYWH(bx, by, bw, clipH);
			const float cGap = 4.0f * s;
			const float cW = (bw - cGap * (out.visibleClipCount - 1)) / out.visibleClipCount;
			for (int i = 0; i < out.visibleClipCount; ++i) {
				out.clipSlotBtns[i] = overlay_compat::MakeRectXYWH(bx + i * (cW + cGap), by, cW, clipH);
			}
		}

		if (statsVisible) {
			float sx = anchorRight ? (mainX - gap - statsW) : (mainX + panelW + gap);
			float sy = gy;
			out.statsPanel = overlay_compat::MakeRectXYWH(sx, sy, statsW, statsH);
		}
	}

	out.hudAnchorBounds = out.mainPanel;
	auto expandBounds = [&](const overlay_compat::RectF &r) {
		if (r.Width() <= 0 || r.Height() <= 0)
			return;
		out.hudAnchorBounds.left = std::min(out.hudAnchorBounds.left, r.left);
		out.hudAnchorBounds.top = std::min(out.hudAnchorBounds.top, r.top);
		out.hudAnchorBounds.right = std::max(out.hudAnchorBounds.right, r.right);
		out.hudAnchorBounds.bottom = std::max(out.hudAnchorBounds.bottom, r.bottom);
	};
	if (statsVisible)
		expandBounds(out.statsPanel);
	if (out.visibleClipCount > 0)
		expandBounds(out.clipPanel);
}
