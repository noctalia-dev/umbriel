// [animation]: shared timing, named curves, and one table per animated event.

#include "config/fields.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    std::optional<BezierCurve> parseBezier(const toml::node& node) {
      const auto* values = node.as_array();
      if (values == nullptr || values->size() != 4) {
        return std::nullopt;
      }
      const auto x1 = (*values)[0].value<double>();
      const auto y1 = (*values)[1].value<double>();
      const auto x2 = (*values)[2].value<double>();
      const auto y2 = (*values)[3].value<double>();
      if (!x1
          || !y1
          || !x2
          || !y2
          || !std::isfinite(*x1)
          || !std::isfinite(*y1)
          || !std::isfinite(*x2)
          || !std::isfinite(*y2)
          || *x1 < 0.0
          || *x1 > 1.0
          || *x2 < 0.0
          || *x2 > 1.0) {
        return std::nullopt;
      }
      return BezierCurve{.x1 = *x1, .y1 = *y1, .x2 = *x2, .y2 = *y2};
    }

    std::optional<SpringConfig> parseSpring(const toml::node& node) {
      const auto* table = node.as_table();
      if (table == nullptr || table->size() != 2) {
        return std::nullopt;
      }
      const toml::node* dampingNode = table->get("damping");
      const toml::node* stiffnessNode = table->get("stiffness");
      if (dampingNode == nullptr || stiffnessNode == nullptr) {
        return std::nullopt;
      }
      const auto damping = dampingNode->value<double>();
      const auto stiffness = stiffnessNode->value<double>();
      if (!damping
          || !stiffness
          || !std::isfinite(*damping)
          || !std::isfinite(*stiffness)
          || *damping < 0.01
          || *damping > 5.0
          || *stiffness < 1.0
          || *stiffness > 10000.0) {
        return std::nullopt;
      }
      return SpringConfig{.damping = *damping, .stiffness = *stiffness};
    }

    std::optional<AnimationCurve> parseAnimationCurve(
        std::string_view str, const std::map<std::string, BezierCurve>& beziers = {},
        const std::map<std::string, SpringConfig>& springs = {}
    ) {
      std::string s = lowercase(str);

      for (const auto& [name, curve] : beziers) {
        if (lowercase(name) == s) {
          return AnimationCurve{.easing = Easing::CustomBezier, .bezier = curve};
        }
      }
      for (const auto& [name, spring] : springs) {
        if (lowercase(name) == s) {
          return AnimationCurve{.easing = Easing::Spring, .spring = spring};
        }
      }

      return CurveRegistry::parse(str);
    }

    std::optional<AnimationCurve> parseCurve(
        const toml::node& node, const std::string& path, const std::map<std::string, BezierCurve>& beziers,
        const std::map<std::string, SpringConfig>& springs
    ) {
      const auto* value = node.as_string();
      if (value == nullptr) {
        warnAt(node.source(), "{} must be a string", path);
        return std::nullopt;
      }
      if (auto curve = parseAnimationCurve(value->get(), beziers, springs)) {
        return curve;
      }
      warnAt(node.source(), R"(invalid curve "{}" in {})", value->get(), path);
      return std::nullopt;
    }

    // Every timeline under [animation], each fed by the shared duration and curve.
    template <typename F> void forEachTimeline(Config::Animation& animation, F&& apply) {
      apply(animation.windowsIn.durationMs, animation.windowsIn.curve);
      apply(animation.windowsOut.durationMs, animation.windowsOut.curve);
      apply(animation.windowsMove.durationMs, animation.windowsMove.curve);
      apply(animation.workspaces.durationMs, animation.workspaces.curve);
      apply(animation.overview.durationMs, animation.overview.curve);
      apply(animation.scratchpad.durationMs, animation.scratchpad.curve);
      apply(animation.border.durationMs, animation.border.curve);
      apply(animation.dimUnfocused.durationMs, animation.dimUnfocused.curve);
      apply(animation.layers.durationMs, animation.layers.curve);
    }

    // A curve key: a built-in easing, or one of the [animation.beziers] and [animation.springs] read before it.
    template <typename T> registry::Field<T> curveField(std::string_view key, AnimationCurve T::* member) {
      return registry::custom<T>(
          key, registry::KeyDescription("string").withFormat("curve"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext& context) {
            const Config::Animation& animation = context.loaded.animation;
            registry::assign(target.*member, parseCurve(node, path, animation.beziers, animation.springs));
          }
      );
    }

    // One animation event: its effect, whether it runs, `extra` keys of its own, and its timeline. duration_ms and
    // curve resolve together, because a spring derives its own length: a duration configured beside one reaches
    // nothing and has to say so rather than look honoured.
    template <typename E> registry::Fields<E> eventFields(registry::Fields<E> extra) {
      registry::Fields<E> fields{
          effectField("effect", &E::effect, EffectKind::Animation),
          registry::boolean("enabled", &E::enabled),
      };
      std::ranges::move(extra, std::back_inserter(fields));
      registry::Field<E> duration = registry::integer("duration_ms", 1, 10000, &E::durationMs);
      duration.describe = [describe = std::move(duration.describe)](
                              const E& defaults, const std::string& path, registry::Descriptions& out
                          ) {
        describe(defaults, path, out);
        // A built-in spring derives its own length, so its duration is not a value to write.
        if (defaults.curve.easing == Easing::Spring) {
          out.back().defaultValue = nullptr;
        }
      };
      fields.push_back(std::move(duration));
      fields.push_back(curveField("curve", &E::curve));
      fields.push_back(registry::step<E>([](Section& s, E& event, registry::ReadContext&) {
        if (registry::configuredInteger(s, "duration_ms") && event.curve.easing == Easing::Spring) {
          warnAt(
              s.node("duration_ms")->source(), "{} has no effect: its spring curve sets its own length",
              s.qualified("duration_ms")
          );
        }
      }));
      return fields;
    }

    template <typename E> registry::Field<E> styleField(std::initializer_list<std::string_view> styles) {
      registry::Choices<std::string> choices;
      for (const std::string_view style : styles) {
        choices.push_back({.name = style, .value = std::string(style)});
      }
      return registry::choice("style", &E::style, std::move(choices));
    }

    const registry::Fields<Config::Animation>& animationFields() {
      using registry::boolean;
      using registry::real;
      using registry::table;
      using A = Config::Animation;
      static const registry::Fields<A::WindowsIn> windowsIn = eventFields<A::WindowsIn>({
          real("scale", 0.1, 1.0, &A::WindowsIn::scale),
          styleField<A::WindowsIn>({"popin", "zoom", "slide", "fade", "none"}),
      });
      static const registry::Fields<A::WindowsOut> windowsOut = eventFields<A::WindowsOut>({
          real("scale", 0.1, 1.0, &A::WindowsOut::scale),
          styleField<A::WindowsOut>({"fade", "slide", "popin", "zoom"}),
      });
      static const registry::Fields<A::WindowsMove> windowsMove = eventFields<A::WindowsMove>({});
      static const registry::Fields<A::Workspaces> workspaces = eventFields<A::Workspaces>({});
      static const registry::Fields<A::Overview> overview = [] {
        auto fields = eventFields<A::Overview>({});
        fields.push_back(curveField("workspace_curve", &A::Overview::workspaceCurve));
        return fields;
      }();
      static const registry::Fields<A::Scratchpad> scratchpad = eventFields<A::Scratchpad>({
          real("dim", 0.0, 1.0, &A::Scratchpad::dim),
          boolean("blur", &A::Scratchpad::blur),
          real("scale", 0.0, 1.0, &A::Scratchpad::scale),
          boolean("maximize", &A::Scratchpad::maximize),
          boolean("fullscreen", &A::Scratchpad::fullscreen),
      });
      static const registry::Fields<A::Border> border = eventFields<A::Border>({});
      static const registry::Fields<A::DimUnfocused> dimUnfocused = eventFields<A::DimUnfocused>({
          real("dim", 0.0, 1.0, &A::DimUnfocused::dim),
      });
      static const registry::Fields<A::Layers> layers = eventFields<A::Layers>({});
      static const registry::Fields<A::WindowsDrag> windowsDrag{
          boolean("physics", &A::WindowsDrag::physics),
      };
      static const registry::Fields<A> fields{
          boolean("enabled", &A::enabled),
          registry::map<A>(
              "beziers", registry::KeyDescription("float_array").withFormat("bezier"),
              [](Section& s, A& animation, registry::ReadContext&) {
                for (const auto& [name, value] : s.table()) {
                  if (auto bezier = parseBezier(value)) {
                    animation.beziers[std::string(name.str())] = *bezier;
                  } else {
                    warnAt(value.source(), "invalid bezier curve '{}'", name.str());
                  }
                }
              }
          ),
          registry::map<A>(
              "springs", registry::KeyDescription("table"),
              [](Section& s, A& animation, registry::ReadContext&) {
                for (const auto& [name, value] : s.table()) {
                  if (auto spring = parseSpring(value)) {
                    animation.springs[std::string(name.str())] = *spring;
                  } else {
                    warnAt(value.source(), "invalid spring config '{}'", name.str());
                  }
                }
              },
              [] {
                registry::Descriptions keys{
                    registry::KeyDescription("float").withRange(0.01, 5.0),
                    registry::KeyDescription("float").withRange(1.0, 10000.0),
                };
                keys[0].path = "damping";
                keys[1].path = "stiffness";
                return keys;
              }()
          ),
          registry::integer("duration_ms", 1, 10000, &A::durationMs),
          registry::step<A>([](Section& s, A& animation, registry::ReadContext&) {
            if (registry::configuredInteger(s, "duration_ms")) {
              forEachTimeline(animation, [&](int& duration, AnimationCurve&) { duration = animation.durationMs; });
            }
          }),
          registry::custom<A>(
              "curve", registry::KeyDescription("string").withFormat("curve"),
              [](const toml::node& node, const std::string& path, A& animation, registry::ReadContext&) {
                if (auto curve = parseCurve(node, path, animation.beziers, animation.springs)) {
                  animation.curve = *curve;
                  forEachTimeline(animation, [&](int&, AnimationCurve& target) { target = *curve; });
                }
              }
          ),
          table("windows_in", &A::windowsIn, windowsIn),
          table("windows_out", &A::windowsOut, windowsOut),
          table("windows_move", &A::windowsMove, windowsMove),
          table("workspaces", &A::workspaces, workspaces),
          table("overview", &A::overview, overview),
          table("scratchpad", &A::scratchpad, scratchpad),
          table("border", &A::border, border),
          table("dim_unfocused", &A::dimUnfocused, dimUnfocused),
          table("layers", &A::layers, layers),
          table("windows_drag", &A::windowsDrag, windowsDrag),
          // The shared duration reaches nothing once every timeline it feeds derives its own length.
          registry::step<A>([](Section& s, A& animation, registry::ReadContext&) {
            if (!registry::configuredInteger(s, "duration_ms")
                || animation.overview.workspaceCurve.easing != Easing::Spring) {
              return;
            }
            bool allSprings = true;
            forEachTimeline(animation, [&](int&, AnimationCurve& curve) {
              allSprings = allSprings && curve.easing == Easing::Spring;
            });
            if (allSprings) {
              warnAt(
                  s.node("duration_ms")->source(),
                  "animation.duration_ms has no effect: every animation curve is a spring"
              );
            }
          }),
      };
      return fields;
    }

  } // namespace

  registry::Field<Config> animationTable() {
    return registry::table("animation", &Config::animation, animationFields());
  }

} // namespace umbriel
