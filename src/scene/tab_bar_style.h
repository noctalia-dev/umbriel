#pragma once

#include "config/config.h"

#include <string>

namespace umbriel {

  // Everything about how a tab bar looks, resolved from [appearance.tab_bar] and [colors.tab_bar]. The bar draws from
  // this alone, so its drawing never reads the global configuration.
  struct TabBarStyle {
    TabBarLook look = TabBarLook::Titles;
    TabBarPosition position = TabBarPosition::Top;
    int height = 24;
    std::string font = "sans 10";
    int padding = 8;
    int tabGap = 2;
    // Already resolved: never -1.
    int cornerRadius = 0;
    TitleAlign titleAlign = TitleAlign::Center;
    Config::Colors::TabBar colors;
    bool operator==(const TabBarStyle&) const = default;
  };

  TabBarStyle resolveTabBarStyle(const Config& config);

} // namespace umbriel
