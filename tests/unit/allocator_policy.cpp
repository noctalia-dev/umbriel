#include "server/allocator_policy.h"

#include "check.h"

using umbriel::AlignedBufferLayout;
using umbriel::alignedBufferLayout;
using umbriel::wantsAlignedSoftwareAllocator;

namespace {
  constexpr long pageSize = 4096;
} // namespace

UMBRIEL_TEST(oddWidthStrideIsPaddedToSixtyFourBytes) {
  // 650 and 1170 px XRGB8888 rows crashed llvmpipe on row 1 with an unaligned stride.
  CHECK_EQ(alignedBufferLayout(650 * 4, 480, pageSize).stride, 2624);
  CHECK_EQ(alignedBufferLayout(1170 * 4, 480, pageSize).stride, 4736);
  CHECK_EQ(alignedBufferLayout(650 * 4, 480, pageSize).stride % 64, 0);
}

UMBRIEL_TEST(alignedStrideIsUnchanged) {
  CHECK_EQ(alignedBufferLayout(1920 * 4, 1080, pageSize).stride, 7680);
  CHECK_EQ(alignedBufferLayout(64, 1, pageSize).stride, 64);
}

UMBRIEL_TEST(heightIsPaddedSoBlockReadsStayInBounds) {
  CHECK_EQ(alignedBufferLayout(256, 1080, pageSize).allocHeight, 1088);
  CHECK_EQ(alignedBufferLayout(256, 1, pageSize).allocHeight, 64);
  CHECK_EQ(alignedBufferLayout(256, 64, pageSize).allocHeight, 64);
}

UMBRIEL_TEST(sizeCoversPaddedRowsAndIsPageRounded) {
  const AlignedBufferLayout layout = alignedBufferLayout(650 * 4, 481, pageSize);
  CHECK(layout.size >= static_cast<std::size_t>(layout.stride) * static_cast<std::size_t>(layout.allocHeight));
  CHECK_EQ(layout.size % pageSize, 0U);
  CHECK_EQ(layout.size, 2624U * 512U);
}

UMBRIEL_TEST(sizeRoundsUpToNextPage) {
  // 64 * 64 = 4096 fits a page exactly; 128 * 64 = 8192 does too; 192 * 64 = 12288 is three pages.
  CHECK_EQ(alignedBufferLayout(64, 64, pageSize).size, 4096U);
  CHECK_EQ(alignedBufferLayout(192, 64, pageSize).size, 12288U);
  CHECK_EQ(alignedBufferLayout(64, 64, 16384).size, 16384U);
}

UMBRIEL_TEST(softwarePathWithoutDrmFdsSelectsAlignedAllocator) { CHECK(wantsAlignedSoftwareAllocator(-1, -1)); }

UMBRIEL_TEST(backendDrmFdKeepsHardwareAllocator) { CHECK(!wantsAlignedSoftwareAllocator(3, -1)); }

UMBRIEL_TEST(rendererDrmFdKeepsHardwareAllocator) { CHECK(!wantsAlignedSoftwareAllocator(-1, 4)); }

UMBRIEL_TEST(bothDrmFdsKeepHardwareAllocator) { CHECK(!wantsAlignedSoftwareAllocator(3, 4)); }

int main() { return RUN_TESTS(); }
