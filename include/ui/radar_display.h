#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft (full frame composite when data changed). */
void radarDisplayRefreshAircraft();

}  // namespace ui
