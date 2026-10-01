#pragma once

namespace umbriel {
  enum class PresentationFallback {
    None,
    UnsupportedCapability,
    SourceUnavailable,
    ResourceBudget,
    CompositionFailure,
    TopologyChanged,
    RendererLost,
    OutputRemoved,
    Locked,
    BindingRemoved,
  };

} // namespace umbriel
