#include <cstdint>
#include <optional>
#include <utility>

namespace umbriel {
  class PageFlipRetry {
  public:
    static constexpr uint8_t kMaxFrameAttempts = 3;

    bool shouldDefer(bool outputFramePending, bool allowReconfiguration) const {
      return outputFramePending && !allowReconfiguration && m_frameAttempts < kMaxFrameAttempts;
    }

    void defer(bool stageGeometry) {
      m_pending = true;
      m_stageGeometry = m_stageGeometry || stageGeometry;
    }

    std::optional<bool> take() {
      if (!m_pending) {
        return std::nullopt;
      }
      m_pending = false;
      ++m_frameAttempts;
      return std::exchange(m_stageGeometry, false);
    }

    void reset() {
      m_pending = false;
      m_stageGeometry = false;
      m_frameAttempts = 0;
    }

  private:
    bool m_pending = false;
    bool m_stageGeometry = false;
    uint8_t m_frameAttempts = 0;
  };
}
