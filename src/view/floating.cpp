#include "view/floating.h"

#include <cmath>

namespace umbriel {

  void FloatingGeometry::rememberPositionFraction(FloatingPoint origin, const wlr_box& usable) {
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }
    m_posFrac = {{
        static_cast<double>(origin.x - usable.x) / usable.width,
        static_cast<double>(origin.y - usable.y) / usable.height,
    }};
  }

  std::optional<FloatingPoint> FloatingGeometry::restoredOrigin(const wlr_box& usable) const {
    if (!m_posFrac || usable.width <= 0 || usable.height <= 0) {
      return std::nullopt;
    }
    return FloatingPoint{
        .x = usable.x + static_cast<int>(std::lround((*m_posFrac)[0] * usable.width)),
        .y = usable.y + static_cast<int>(std::lround((*m_posFrac)[1] * usable.height)),
    };
  }

  bool FloatingGeometry::retireSizeRequestIfSettled(uint32_t committedSerial) {
    if (!m_sizeRequestSerial) {
      return true;
    }
    if (!serialSettled(committedSerial, *m_sizeRequestSerial)) {
      return false;
    }
    m_sizeRequestSerial.reset();
    m_pendingSize.reset();
    // A resize still under the pointer keeps its anchor: more configures are coming.
    if (!m_resizeActive) {
      clearAnchor();
    }
    return true;
  }

  void FloatingGeometry::beginResize(const wlr_box& anchor, uint32_t edges) {
    m_anchor = anchor;
    m_edges = edges;
    m_resizeActive = true;
  }

  void FloatingGeometry::endResize() {
    m_resizeActive = false;
    // Drop the anchor only if nothing is still in flight; otherwise the last
    // configure's commit needs it to place the window.
    if (!m_sizeRequestSerial) {
      clearAnchor();
    }
  }

  void FloatingGeometry::clearAnchor() {
    m_anchor.reset();
    m_edges = 0;
  }

} // namespace umbriel
