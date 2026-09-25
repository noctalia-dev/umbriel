// Harness helper that answers pixel questions about a screenshot. Colour channels are 0..1 in predicates and 0..255 in
// printed colours. A region is WxH+X+Y, clipped to the image; without one the whole image is used.
//
//   pixel-probe <png> count <predicate>... [region]  matching pixels, one count per predicate
//   pixel-probe <png> bbox <predicate> [region]    "x y w h" of matching pixels, "0 0 0 0" when none match
//   pixel-probe <png> mean [region]                "r g b" region mean
//   pixel-probe <png> max [region]                 "r g b" per-channel maximum
//   pixel-probe <png> pixel <x> <y>                "r g b"
//   pixel-probe <png> size                         "WxH"
//
// A predicate compares r, g, b and numbers with < <= > >= == !=, combined with && || ! and parentheses, for example
// '(r > 0.8 && g < 0.1) || b > 0.5'.

#include <algorithm>
#include <cairo.h>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  // Single precision, like ImageMagick's channels: a channel of 51/255 then compares as slightly above 0.2, which is
  // how the thresholds in existing checks were tuned.
  struct Rgb {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
  };

  struct Image {
    int width = 0;
    int height = 0;
    std::vector<Rgb> pixels;
    [[nodiscard]] const Rgb& at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
  };

  struct Region {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  std::optional<Image> load(const char* path) {
    cairo_surface_t* surface = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      return std::nullopt;
    }
    cairo_surface_flush(surface);
    const cairo_format_t format = cairo_image_surface_get_format(surface);
    Image image{
        .width = cairo_image_surface_get_width(surface),
        .height = cairo_image_surface_get_height(surface),
        .pixels = {},
    };
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    image.pixels.reserve(static_cast<size_t>(image.width) * image.height);
    for (int y = 0; y < image.height; ++y) {
      const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<ptrdiff_t>(y) * stride);
      for (int x = 0; x < image.width; ++x) {
        const uint32_t argb = row[x];
        const uint32_t alpha = format == CAIRO_FORMAT_ARGB32 ? argb >> 24 : 255;
        // Cairo stores premultiplied colour; report the straight colour the PNG holds.
        const auto channel = [alpha](uint32_t value) {
          if (alpha == 0) {
            return 0.0F;
          }
          return static_cast<float>(std::min(255.0, std::round(value * 255.0 / alpha)) / 255.0);
        };
        image.pixels.push_back({
            .r = channel((argb >> 16) & 0xff),
            .g = channel((argb >> 8) & 0xff),
            .b = channel(argb & 0xff),
        });
      }
    }
    cairo_surface_destroy(surface);
    return image;
  }

  std::optional<int> parseInt(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<Region> parseRegion(std::string_view text, const Image& image) {
    const size_t cross = text.find('x');
    const size_t plusX = text.find('+', cross);
    const size_t plusY = plusX == std::string_view::npos ? plusX : text.find('+', plusX + 1);
    if (cross == std::string_view::npos || plusX == std::string_view::npos || plusY == std::string_view::npos) {
      return std::nullopt;
    }
    const auto width = parseInt(text.substr(0, cross));
    const auto height = parseInt(text.substr(cross + 1, plusX - cross - 1));
    const auto x = parseInt(text.substr(plusX + 1, plusY - plusX - 1));
    const auto y = parseInt(text.substr(plusY + 1));
    if (!width || !height || !x || !y || *width < 0 || *height < 0) {
      return std::nullopt;
    }
    const int left = std::clamp(*x, 0, image.width);
    const int top = std::clamp(*y, 0, image.height);
    const int right = std::clamp(*x + *width, 0, image.width);
    const int bottom = std::clamp(*y + *height, 0, image.height);
    return Region{.x = left, .y = top, .width = right - left, .height = bottom - top};
  }

  // Predicates compile to a small tree evaluated once per pixel.
  struct Node {
    enum class Kind : uint8_t {
      Number,
      Red,
      Green,
      Blue,
      Not,
      And,
      Or,
      Less,
      LessEq,
      Greater,
      GreaterEq,
      Equal,
      NotEqual
    };
    Kind kind = Kind::Number;
    double number = 0.0;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
  };

  class Parser {
  public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::unique_ptr<Node> parse() {
      auto node = parseOr();
      skipSpace();
      if (node == nullptr || m_pos != m_text.size()) {
        return nullptr;
      }
      return node;
    }

  private:
    void skipSpace() {
      while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])) != 0) {
        ++m_pos;
      }
    }

    bool accept(std::string_view token) {
      skipSpace();
      if (m_text.substr(m_pos, token.size()) == token) {
        m_pos += token.size();
        return true;
      }
      return false;
    }

    static std::unique_ptr<Node> binary(Node::Kind kind, std::unique_ptr<Node> left, std::unique_ptr<Node> right) {
      if (left == nullptr || right == nullptr) {
        return nullptr;
      }
      auto node = std::make_unique<Node>();
      node->kind = kind;
      node->left = std::move(left);
      node->right = std::move(right);
      return node;
    }

    std::unique_ptr<Node> parseOr() {
      auto node = parseAnd();
      while (node != nullptr && accept("||")) {
        node = binary(Node::Kind::Or, std::move(node), parseAnd());
      }
      return node;
    }

    std::unique_ptr<Node> parseAnd() {
      auto node = parseCompare();
      while (node != nullptr && accept("&&")) {
        node = binary(Node::Kind::And, std::move(node), parseCompare());
      }
      return node;
    }

    std::unique_ptr<Node> parseCompare() {
      auto left = parseUnary();
      if (left == nullptr) {
        return nullptr;
      }
      // Two-character operators first, so "<=" is not read as "<".
      constexpr std::pair<std::string_view, Node::Kind> kOperators[] = {
          {"<=", Node::Kind::LessEq},   {">=", Node::Kind::GreaterEq}, {"==", Node::Kind::Equal},
          {"!=", Node::Kind::NotEqual}, {"<", Node::Kind::Less},       {">", Node::Kind::Greater},
      };
      for (const auto& [token, kind] : kOperators) {
        if (accept(token)) {
          return binary(kind, std::move(left), parseUnary());
        }
      }
      return left;
    }

    std::unique_ptr<Node> parseUnary() {
      skipSpace();
      if (m_pos < m_text.size() && m_text[m_pos] == '!' && m_text.substr(m_pos, 2) != "!=") {
        ++m_pos;
        auto operand = parseUnary();
        if (operand == nullptr) {
          return nullptr;
        }
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Not;
        node->left = std::move(operand);
        return node;
      }
      if (accept("(")) {
        auto node = parseOr();
        return accept(")") ? std::move(node) : nullptr;
      }
      auto node = std::make_unique<Node>();
      for (const auto& [name, kind] :
           {std::pair{'r', Node::Kind::Red}, std::pair{'g', Node::Kind::Green}, std::pair{'b', Node::Kind::Blue}}) {
        if (m_pos < m_text.size() && m_text[m_pos] == name) {
          ++m_pos;
          node->kind = kind;
          return node;
        }
      }
      const char* begin = m_text.data() + m_pos;
      double value = 0.0;
      const auto [end, error] = std::from_chars(begin, m_text.data() + m_text.size(), value);
      if (error != std::errc{}) {
        return nullptr;
      }
      m_pos += static_cast<size_t>(end - begin);
      node->kind = Node::Kind::Number;
      node->number = value;
      return node;
    }

    std::string_view m_text;
    size_t m_pos = 0;
  };

  double evaluate(const Node& node, const Rgb& pixel) {
    using K = Node::Kind;
    switch (node.kind) {
    case K::Number:
      return node.number;
    case K::Red:
      return pixel.r;
    case K::Green:
      return pixel.g;
    case K::Blue:
      return pixel.b;
    case K::Not:
      return evaluate(*node.left, pixel) == 0.0 ? 1.0 : 0.0;
    case K::And:
      return evaluate(*node.left, pixel) != 0.0 && evaluate(*node.right, pixel) != 0.0 ? 1.0 : 0.0;
    case K::Or:
      return evaluate(*node.left, pixel) != 0.0 || evaluate(*node.right, pixel) != 0.0 ? 1.0 : 0.0;
    case K::Less:
      return evaluate(*node.left, pixel) < evaluate(*node.right, pixel) ? 1.0 : 0.0;
    case K::LessEq:
      return evaluate(*node.left, pixel) <= evaluate(*node.right, pixel) ? 1.0 : 0.0;
    case K::Greater:
      return evaluate(*node.left, pixel) > evaluate(*node.right, pixel) ? 1.0 : 0.0;
    case K::GreaterEq:
      return evaluate(*node.left, pixel) >= evaluate(*node.right, pixel) ? 1.0 : 0.0;
    case K::Equal:
      return evaluate(*node.left, pixel) == evaluate(*node.right, pixel) ? 1.0 : 0.0;
    case K::NotEqual:
      return evaluate(*node.left, pixel) != evaluate(*node.right, pixel) ? 1.0 : 0.0;
    }
    return 0.0;
  }

  int channel255(double value) { return static_cast<int>(std::lround(value * 255.0)); }

  int usage() {
    std::println(stderr, "usage: pixel-probe <png> count <predicate>... [WxH+X+Y]");
    std::println(stderr, "       pixel-probe <png> bbox <predicate> [WxH+X+Y]");
    std::println(stderr, "       pixel-probe <png> mean|max [WxH+X+Y]");
    std::println(stderr, "       pixel-probe <png> pixel <x> <y>");
    std::println(stderr, "       pixel-probe <png> size");
    return 2;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return usage();
  }
  const std::optional<Image> image = load(argv[1]);
  if (!image) {
    std::println(stderr, "pixel-probe: cannot read PNG '{}'", argv[1]);
    return 1;
  }
  const std::string_view op = argv[2];
  const std::vector<std::string_view> args(argv + 3, argv + argc);

  const auto regionArg = [&](size_t index) -> std::optional<Region> {
    if (args.size() <= index) {
      return Region{.x = 0, .y = 0, .width = image->width, .height = image->height};
    }
    return parseRegion(args[index], *image);
  };

  if (op == "size" && args.empty()) {
    std::println("{}x{}", image->width, image->height);
    return 0;
  }
  if (op == "pixel" && args.size() == 2) {
    const auto x = parseInt(args[0]);
    const auto y = parseInt(args[1]);
    if (!x || !y || *x < 0 || *y < 0 || *x >= image->width || *y >= image->height) {
      std::println(stderr, "pixel-probe: pixel outside the image");
      return 1;
    }
    const Rgb& pixel = image->at(*x, *y);
    std::println("{} {} {}", channel255(pixel.r), channel255(pixel.g), channel255(pixel.b));
    return 0;
  }
  if ((op == "mean" || op == "max") && args.size() <= 1) {
    const std::optional<Region> region = regionArg(0);
    if (!region || region->width == 0 || region->height == 0) {
      std::println(stderr, "pixel-probe: empty or malformed region");
      return 1;
    }
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (int y = region->y; y < region->y + region->height; ++y) {
      for (int x = region->x; x < region->x + region->width; ++x) {
        const Rgb& pixel = image->at(x, y);
        if (op == "mean") {
          red += pixel.r;
          green += pixel.g;
          blue += pixel.b;
        } else {
          red = std::max<double>(red, pixel.r);
          green = std::max<double>(green, pixel.g);
          blue = std::max<double>(blue, pixel.b);
        }
      }
    }
    if (op == "mean") {
      const double count = static_cast<double>(region->width) * region->height;
      red /= count;
      green /= count;
      blue /= count;
    }
    std::println("{} {} {}", channel255(red), channel255(green), channel255(blue));
    return 0;
  }
  if (op == "count" || op == "bbox") {
    // A trailing argument that parses as a region is the region; every other argument is a predicate.
    std::vector<std::string_view> predicateArgs = args;
    Region region{.x = 0, .y = 0, .width = image->width, .height = image->height};
    if (!predicateArgs.empty()) {
      if (const auto trailing = parseRegion(predicateArgs.back(), *image)) {
        region = *trailing;
        predicateArgs.pop_back();
      }
    }
    if (predicateArgs.empty() || (op == "bbox" && predicateArgs.size() != 1)) {
      return usage();
    }
    std::vector<std::unique_ptr<Node>> predicates;
    for (const std::string_view text : predicateArgs) {
      predicates.push_back(Parser(text).parse());
      if (predicates.back() == nullptr) {
        std::println(stderr, "pixel-probe: malformed predicate '{}'", text);
        return 2;
      }
    }
    std::vector<long> counts(predicates.size(), 0);
    int left = image->width;
    int top = image->height;
    int right = -1;
    int bottom = -1;
    for (int y = region.y; y < region.y + region.height; ++y) {
      for (int x = region.x; x < region.x + region.width; ++x) {
        const Rgb& pixel = image->at(x, y);
        for (size_t index = 0; index < predicates.size(); ++index) {
          if (evaluate(*predicates[index], pixel) != 0.0) {
            ++counts[index];
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
          }
        }
      }
    }
    if (op == "count") {
      std::string line;
      for (const long count : counts) {
        line += (line.empty() ? "" : " ") + std::to_string(count);
      }
      std::println("{}", line);
    } else if (counts.front() == 0) {
      std::println("0 0 0 0");
    } else {
      std::println("{} {} {} {}", left, top, right - left + 1, bottom - top + 1);
    }
    return 0;
  }
  return usage();
}
