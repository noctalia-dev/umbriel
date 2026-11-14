#include "config/config_diag.h"

#include <utility>

namespace umbriel {

  namespace {

    DiagnosticObserver gObserver = nullptr;

  } // namespace

  void setDiagnosticObserver(DiagnosticObserver observer) { gObserver = observer; }

  void recordDiagnostic(std::vector<ConfigDiagnostic>& sink, ConfigDiagnostic diagnostic) {
    if (gObserver != nullptr) {
      gObserver(diagnostic);
    }
    sink.push_back(std::move(diagnostic));
  }

} // namespace umbriel
