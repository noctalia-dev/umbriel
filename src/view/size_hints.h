#pragma once

#include "wlr.h"

#include <algorithm>

namespace umbriel {

  struct SizeHints {
    int minWidth = 1;
    int minHeight = 1;
    int maxWidth = 0;  // 0 = unlimited
    int maxHeight = 0; // 0 = unlimited
  };

  inline SizeHints normalizedSizeHints(SizeHints hints) {
    if (hints.maxWidth > 0 && hints.maxWidth < hints.minWidth) {
      hints.maxWidth = hints.minWidth;
    }
    if (hints.maxHeight > 0 && hints.maxHeight < hints.minHeight) {
      hints.maxHeight = hints.minHeight;
    }
    return hints;
  }

  inline SizeHints xdgSizeHints(const wlr_xdg_toplevel* toplevel) {
    SizeHints hints;
    if (toplevel == nullptr) {
      return hints;
    }
    const auto& state = toplevel->current;
    if (state.min_width > 0) {
      hints.minWidth = state.min_width;
    }
    if (state.min_height > 0) {
      hints.minHeight = state.min_height;
    }
    if (state.max_width > 0) {
      hints.maxWidth = state.max_width;
    }
    if (state.max_height > 0) {
      hints.maxHeight = state.max_height;
    }
    return normalizedSizeHints(hints);
  }

  // WM_NORMAL_HINTS of an X11 window. Only the program-specified min and max sizes constrain the window.
  inline SizeHints x11SizeHints(const xcb_size_hints_t* sizeHints) {
    SizeHints hints;
    if (sizeHints == nullptr) {
      return hints;
    }
    if ((sizeHints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0) {
      if (sizeHints->min_width > 0) {
        hints.minWidth = sizeHints->min_width;
      }
      if (sizeHints->min_height > 0) {
        hints.minHeight = sizeHints->min_height;
      }
    }
    if ((sizeHints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) != 0) {
      if (sizeHints->max_width > 0) {
        hints.maxWidth = sizeHints->max_width;
      }
      if (sizeHints->max_height > 0) {
        hints.maxHeight = sizeHints->max_height;
      }
    }
    return normalizedSizeHints(hints);
  }

  inline int clampWidth(int width, const SizeHints& hints) {
    width = std::max(width, hints.minWidth);
    if (hints.maxWidth > 0) {
      width = std::min(width, hints.maxWidth);
    }
    return std::max(1, width);
  }

  inline int clampHeight(int height, const SizeHints& hints) {
    height = std::max(height, hints.minHeight);
    if (hints.maxHeight > 0) {
      height = std::min(height, hints.maxHeight);
    }
    return std::max(1, height);
  }

} // namespace umbriel
