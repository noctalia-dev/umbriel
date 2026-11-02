#pragma once

#include "wlr_color.h"

#include <utility>

extern "C" {
#include <wlr/types/wlr_output.h>
}

namespace umbriel {

  enum class [[nodiscard]] RenderedStateCommitResult {
    BuildFailed,
    MissingBuffer,
    CommitFailed,
    Committed,
  };

  template <typename Build, typename Commit>
  RenderedStateCommitResult commitRenderedState(wlr_output_state& state, Build&& build, Commit&& commit) {
    if (!std::forward<Build>(build)(state)) {
      return RenderedStateCommitResult::BuildFailed;
    }
    if ((state.committed & WLR_OUTPUT_STATE_BUFFER) == 0 || state.buffer == nullptr) {
      return RenderedStateCommitResult::MissingBuffer;
    }
    return std::forward<Commit>(commit)(state) ? RenderedStateCommitResult::Committed
                                               : RenderedStateCommitResult::CommitFailed;
  }

} // namespace umbriel
