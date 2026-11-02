#pragma once

namespace umbriel {

  // HDR policy changes are committed with the next rendered output frame. A
  // failed commit leaves the request pending so a later frame can retry it.
  class HdrTransition {
  public:
    bool pending(bool outputEnabled, bool requested) const { return outputEnabled && requested != m_committedRequest; }

    void recordCommit(bool outputEnabled, bool requested, bool succeeded) {
      if (outputEnabled && succeeded) {
        m_committedRequest = requested;
      }
    }

    bool committedRequest() const { return m_committedRequest; }

  private:
    bool m_committedRequest = false;
  };

} // namespace umbriel
