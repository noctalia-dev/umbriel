#include "config/nine_rect.h"

#include "check.h"
#include "config/section.h"

#include <filesystem>
#include <png.h>
#include <unistd.h>

using namespace umbriel;
namespace {
  struct Fixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("umbriel-nine-test-" + std::to_string(getpid()));
    Fixture() {
      std::filesystem::create_directories(dir);
      write("frame.png", 6, 6);
    }
    ~Fixture() { std::filesystem::remove_all(dir); }
    void write(const char* name, int w, int h) {
      FILE* file = fopen((dir / name).c_str(), "wb");
      auto png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
      auto info = png_create_info_struct(png);
      png_init_io(png, file);
      png_set_IHDR(
          png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
          PNG_FILTER_TYPE_DEFAULT
      );
      png_set_gAMA(png, info, 0.45455);
      png_write_info(png, info);
      std::vector<uint8_t> row(w * 4);
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          auto* p = row.data() + x * 4;
          p[0] = 200;
          p[1] = 100;
          p[2] = 50;
          p[3] = 128;
        }
        png_write_row(png, row.data());
      }
      png_write_end(png, info);
      png_destroy_write_struct(&png, &info);
      fclose(file);
    }
    NineRectConfig load(std::string extra) {
      auto table = toml::parse("texture = 'frame.png'\n" + extra, (dir / "config.toml").string());
      std::vector<ConfigDiagnostic> diagnostics;
      Section section(table, "appearance.nine_rect", diagnostics);
      return readNineRect(section, true);
    }
  };
  constexpr auto slices = "slice_px = {top=2, bottom=2, left=2, right=2}\n";
} // namespace
UMBRIEL_TEST(fractional_slices_and_rectangular_frame) {
  Fixture f;
  auto a = f.load("slice = {top=0.333333, bottom=0.333333, left=0.333333, right=0.333333}\n").asset;
  CHECK_EQ(a->slices, (FrameInsets{2, 2, 2, 2}));
  CHECK_EQ(a->content(), a->slices);
  CHECK(!a->overlay);
  CHECK_EQ(a->pixels[0], 0x80643219u);
}
UMBRIEL_TEST(invalid_assets_and_slices_rejected) {
  Fixture f;
  for (const auto& extra :
       {std::string(slices) + "slice = {top=0, bottom=0, left=0, right=0}",
        std::string("slice_px = {top=-1, bottom=2, left=2, right=2}"),
        std::string("slice_px = {top=2, bottom=2, left=4, right=4}"),
        std::string("slice = {top=0.6, bottom=0.6, left=0, right=0}"),
        std::string("slice_px = {top=2.5, bottom=2, left=2, right=2}"),
        std::string(slices) + "center_mode = 'underlay'", std::string(slices) + "scale = 1.5",
        std::string("slice_px = {top=2, bottom=2, left=2}"), std::string(slices) + "mask='removed.png'",
        std::string(slices) + "content_inset={top=-0.1,bottom=0,left=0,right=0}",
        std::string(slices) + "content_inset={top=nan,bottom=0,left=0,right=0}",
        std::string(slices) + "content_inset={top=inf,bottom=0,left=0,right=0}",
        std::string(slices) + "content_inset={top=1.1,bottom=0,left=0,right=0}",
        std::string(slices) + "content_inset={top=0.5,bottom=0,left=0,right=0}",
        std::string(slices) + "content_inset={top=0,bottom=0,left=0}",
        std::string(slices) + "content_inset_px={top=0.5,bottom=0,left=0,right=0}",
        std::string(slices)
            + "content_inset={top=0,bottom=0,left=0,right=0}\ncontent_inset_px={top=0,bottom=0,left=0,right=0}"}) {
    bool failed = false;
    try {
      (void)f.load(extra);
    } catch (const std::exception&) {
      failed = true;
    }
    CHECK(failed);
  }
}
UMBRIEL_TEST(content_insets_preserve_slices_and_fill_inner_padding) {
  Fixture f;
  const auto a = f.load(std::string(slices) + "content_inset_px={top=1,bottom=1,left=1,right=1}\n").asset;
  CHECK_EQ(a->slices, (FrameInsets{2, 2, 2, 2}));
  CHECK_EQ(a->content(), (FrameInsets{1, 1, 1, 1}));
  CHECK_EQ(a->destinationCuts(10, true), (std::array<int, 4>{-1, 1, 9, 11}));
  CHECK_EQ(a->destinationCuts(1, true), (std::array<int, 4>{-1, 1, 1, 2}));
  bool rejected = false;
  try {
    f.load(std::string(slices) + "content_inset_px={top=3,bottom=1,left=1,right=1}\n");
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);
}
UMBRIEL_TEST(tint_is_optional_and_requires_a_boolean) {
  Fixture f;
  CHECK(!f.load(slices).asset->tintWithBorderColor);
  CHECK(f.load(std::string(slices) + "tint_with_border_color=true\n").asset->tintWithBorderColor);
  bool rejected = false;
  try {
    f.load(std::string(slices) + "tint_with_border_color='true'\n");
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);
}
UMBRIEL_TEST(fractional_content_insets_use_source_axes_and_half_up_rounding) {
  Fixture f;
  f.write("frame.png", 12, 8);
  auto a = f.load(
                "slice_px={top=4,bottom=4,left=4,right=4}\n"
                "content_inset={top=0.3125,bottom=0.125,left=0.125,right=0.25}\n"
  )
               .asset;
  CHECK_EQ(a->content(), (FrameInsets{3, 1, 2, 3}));
  auto b = f.load(
                "slice_px={top=4,bottom=4,left=4,right=4}\n"
                "content_inset_px={top=3,bottom=1,left=2,right=3}\n"
  )
               .asset;
  CHECK_EQ(*a, *b);
  a = f.load(
           "slice={top=0.5,bottom=0.5,left=0.333333,right=0.333333}\n"
           "content_inset={top=0,bottom=0,left=0,right=0}\n"
  )
          .asset;
  CHECK_EQ(a->content(), (FrameInsets{}));
}
int main() { return umbriel::test::runAll(); }
