#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace umbriel {

  enum class AudioProvider : std::uint8_t { Pipewire, External };
  enum class AudioMode : std::uint8_t { Playback, Microphone };

  struct AudioSource {
    std::string name;
    AudioProvider provider = AudioProvider::Pipewire;
    AudioMode mode = AudioMode::Playback;
    std::string target;
    bool followDefault = false;
    // External providers are direct exec, with no shell expansion. Relative
    // executable paths resolve beside their declaring TOML value.
    std::string executable;
    std::vector<std::string> args;
    bool operator==(const AudioSource&) const = default;
  };

} // namespace umbriel
