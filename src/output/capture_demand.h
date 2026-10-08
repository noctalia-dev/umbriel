#pragma once

namespace umbriel {
  struct OutputCaptureConsumer {
    const void* source = nullptr; // null means this consumer belongs to another output
    bool pending = false;
  };

  // wlroots holds one render lock per distinct output source, not per session.
  // Unknown locks retain the conservative capture behavior (e.g. screencopy).
  template <typename Iterator, typename Observe>
  bool outputCaptureRequested(int renderLocks, Iterator begin, Iterator end, Observe observe) {
    if (renderLocks <= 0) {
      return false;
    }
    for (auto current = begin; current != end; ++current) {
      const OutputCaptureConsumer consumer = observe(*current);
      if (consumer.source == nullptr) {
        continue;
      }
      if (consumer.pending) {
        return true;
      }
      bool first = true;
      for (auto previous = begin; previous != current; ++previous) {
        if (observe(*previous).source == consumer.source) {
          first = false;
          break;
        }
      }
      if (first) {
        --renderLocks;
      }
    }
    return renderLocks > 0;
  }
} // namespace umbriel
