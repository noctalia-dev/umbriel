#include "audio/bindings.h"
#include "check.h"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
  using namespace umbriel;
  using namespace umbriel::audio;

  struct Loop {
    wl_event_loop* value = wl_event_loop_create();
    ~Loop() { wl_event_loop_destroy(value); }
  };

  AudioSource source(std::string name) {
    AudioSource result;
    result.name = std::move(name);
    result.provider = AudioProvider::External;
    result.mode = AudioMode::Playback;
    result.target = "explicit-test-source";
    result.executable = UMBRIEL_AUDIO_SYNTHETIC;
    result.args = {"--external-test", "--silence"};
    return result;
  }

  bool waitForInput(Loop& loop, Bindings& bindings, std::string_view name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto* value = bindings.inspect(name);
      if (value != nullptr && value->input().available) {
        return true;
      }
      if (wl_event_loop_dispatch(loop.value, 10) < 0) {
        break;
      }
    }
    return false;
  }
} // namespace

UMBRIEL_TEST(declarationsInspectionAndHiddenOwnersDoNotDemandAudio) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int output = 0;
  bindings.configure({source("desktop")});
  CHECK_EQ(bindings.demandedSources(), 0U);
  CHECK(bindings.inspect("desktop") == nullptr);
  CHECK(!bindings.active(&output));
  CHECK(!bindings.dirty(&output));
  bindings.update(&owner, &output, "desktop", false);
  CHECK_EQ(bindings.demandedSources(), 0U);
  CHECK(bindings.inspect("desktop") == nullptr);
  bindings.remove(&owner);
}

UMBRIEL_TEST(failedSubmissionsRetryOneSnapshotAndFreezeAcceptsExplicitInjection) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int output = 0;
  bindings.configure({source("desktop")});
  bindings.update(&owner, &output, "desktop", true);
  CHECK(waitForInput(loop, bindings, "desktop"));
  CHECK(bindings.inject("desktop", Features{.rms = 0.25F}));
  bindings.begin(&output, true, false);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.25F);
  bindings.finish(&output, false);
  CHECK(bindings.inject("desktop", Features{.rms = 0.75F}));
  bindings.begin(&output, true, false);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.25F);
  CHECK(bindings.dirty(&output));
  bindings.finish(&output, true);
  bindings.begin(&output, true, false);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.75F);
  bindings.finish(&output, true);
  CHECK(!bindings.dirty(&output));
  bindings.begin(&output, true, true);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.75F);
  CHECK(bindings.inject("desktop", Features{.rms = 0.5F}));
  bindings.begin(&output, true, true);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.5F);
  bindings.finish(&output, true);
  bindings.setSessionActive(false);
  CHECK_EQ(bindings.input(&output, "desktop"), Input{});
  CHECK(!bindings.dirty(&output));
  bindings.remove(&owner);
}

UMBRIEL_TEST(migrationAndSourceReplacementDoNotLeaveOldOutputDirty) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int otherOwner = 0;
  int firstOutput = 0;
  int secondOutput = 0;
  bindings.configure({source("desktop"), source("voice")});
  bindings.update(&owner, &firstOutput, "desktop", true);
  bindings.update(&otherOwner, &firstOutput, "desktop", true);
  CHECK_EQ(bindings.demandedSources(), 1U);
  CHECK(waitForInput(loop, bindings, "desktop"));
  CHECK(bindings.inject("desktop", Features{.rms = 1}));
  bindings.begin(&firstOutput, true, false);
  bindings.finish(&firstOutput, false);
  bindings.remove(&otherOwner);
  bindings.update(&owner, &secondOutput, "voice", true);
  CHECK(!bindings.active(&firstOutput));
  CHECK(!bindings.dirty(&firstOutput));
  CHECK_EQ(bindings.input(&firstOutput, "desktop"), Input{});
  CHECK_EQ(bindings.demandedSources(), 1U);
  CHECK(bindings.active(&secondOutput));
  bindings.update(&owner, &secondOutput, "voice", false);
  CHECK_EQ(bindings.demandedSources(), 0U);
  CHECK(!bindings.dirty(&secondOutput));
  bindings.removeOutput(&secondOutput);
}

UMBRIEL_TEST(reconfigurationRebuildsLiveLatchesAndKeepsSelectionOwners) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int output = 0;
  auto desktop = source("desktop");
  bindings.configure({desktop});
  bindings.update(&owner, &output, "desktop", true);
  CHECK(waitForInput(loop, bindings, "desktop"));
  CHECK(bindings.inject("desktop", Features{.rms = 0.25F}));
  bindings.begin(&output, true, false);
  bindings.finish(&output, false);
  desktop.target = "changed-explicit-test-source";
  bindings.configure({desktop});
  CHECK(waitForInput(loop, bindings, "desktop"));
  CHECK(bindings.inject("desktop", Features{.rms = 0.75F}));
  bindings.begin(&output, true, false);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.75F);
  bindings.finish(&output, true);
  CHECK_EQ(bindings.demandedSources(), 1U);
  bindings.configure({});
  CHECK_EQ(bindings.demandedSources(), 0U);
  CHECK_EQ(bindings.input(&output, "desktop"), Input{});
  CHECK(!bindings.dirty(&output));
}

UMBRIEL_TEST(reconfigurationAllowsGracefulEofBeforeStartingReplacement) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int output = 0;
  const auto marker = std::filesystem::temp_directory_path() / ("umbriel-audio-eof-" + std::to_string(getpid()));
  std::filesystem::remove(marker);
  auto desktop = source("desktop");
  desktop.args.insert(desktop.args.end(), {"--eof-marker", marker.string()});
  bindings.configure({desktop});
  bindings.update(&owner, &output, "desktop", true);
  CHECK(waitForInput(loop, bindings, "desktop"));
  const auto epoch = bindings.inspect("desktop")->epoch();
  desktop.target = "replacement";
  bindings.configure({desktop});
  CHECK_EQ(bindings.state("desktop"), "retiring");
  CHECK_EQ(bindings.demandedSources(), 0U);
  desktop.target = "coalesced-replacement";
  bindings.configure({desktop});
  CHECK(waitForInput(loop, bindings, "desktop"));
  CHECK(bindings.inspect("desktop")->epoch() > epoch);
  std::ifstream result(marker);
  std::string text;
  std::getline(result, text);
  CHECK_EQ(text, "EOF");
  std::filesystem::remove(marker);
  bindings.configure({});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (bindings.state("desktop") == "retiring" && std::chrono::steady_clock::now() < deadline) {
    CHECK(wl_event_loop_dispatch(loop.value, 10) >= 0);
  }
  std::filesystem::remove(marker);
}

UMBRIEL_TEST(oneOwnerHasIndependentOutputOccurrenceLatches) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int first = 0;
  int second = 0;
  const std::array<const void*, 2> outputs{&first, &second};
  bindings.configure({source("desktop")});
  bindings.updateOccurrences(&owner, outputs, "desktop", true);
  CHECK(waitForInput(loop, bindings, "desktop"));
  const auto epoch = bindings.inspect("desktop")->epoch();
  CHECK_EQ(bindings.demandedSources(), 1U);
  CHECK(bindings.inject("desktop", Features{.rms = 0.25F}));
  bindings.begin(&first, true, false);
  bindings.finish(&first, false);
  CHECK(bindings.inject("desktop", Features{.rms = 0.75F}));
  bindings.begin(&second, true, false);
  bindings.finish(&second, true);
  CHECK_EQ(bindings.input(&first, "desktop").features.rms, 0.25F);
  CHECK_EQ(bindings.input(&second, "desktop").features.rms, 0.75F);
  CHECK(bindings.dirty(&first));
  CHECK(!bindings.dirty(&second));
  bindings.removeOutput(&first);
  CHECK(!bindings.active(&first));
  CHECK(bindings.active(&second));
  CHECK_EQ(bindings.inspect("desktop")->epoch(), epoch);
  bindings.remove(&owner);
  CHECK_EQ(bindings.demandedSources(), 0U);
}

UMBRIEL_TEST(frozenInjectionRemainsScheduledAcrossFailedOlderSubmission) {
  Loop loop;
  Bindings bindings(loop.value);
  int owner = 0;
  int output = 0;
  bindings.configure({source("desktop")});
  bindings.update(&owner, &output, "desktop", true);
  CHECK(waitForInput(loop, bindings, "desktop"));
  CHECK(bindings.inject("desktop", Features{.rms = 0.25F}));
  bindings.begin(&output, true, true);
  const auto firstRevision = bindings.inputRevision(&output);
  CHECK(firstRevision > 0);
  bindings.finish(&output, false);
  CHECK(bindings.inject("desktop", Features{.rms = 0.75F}));
  bindings.begin(&output, true, true);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.25F);
  CHECK_EQ(bindings.inputRevision(&output), firstRevision);
  bindings.finish(&output, true);
  CHECK(bindings.injected(&output));
  bindings.begin(&output, true, true);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.75F);
  CHECK(bindings.inputRevision(&output) > firstRevision);
  const auto secondRevision = bindings.inputRevision(&output);
  bindings.finish(&output, true);
  CHECK(!bindings.injected(&output));
  CHECK(!bindings.dirty(&output));
  bindings.begin(&output, false, true);
  CHECK_EQ(bindings.inputRevision(&output), secondRevision);
  bindings.finish(&output, true);
  bindings.clearInjections();
  bindings.begin(&output, true, false);
  CHECK(bindings.inputRevision(&output) > secondRevision);
  CHECK_EQ(bindings.input(&output, "desktop").features.rms, 0.0F);
}

UMBRIEL_TEST(occurrenceReplacementAtSourceCapReleasesBeforeAcquiring) {
  Loop loop;
  Bindings bindings(loop.value);
  std::vector<AudioSource> definitions;
  definitions.reserve(8);
  for (int i = 0; i < 8; ++i) {
    definitions.push_back(source("source" + std::to_string(i)));
  }
  bindings.configure(definitions);
  std::array<int, 8> owners{};
  int output = 0;
  for (int i = 0; i < 4; ++i) {
    bindings.update(&owners[i], &output, definitions[i].name, true);
  }
  CHECK_EQ(bindings.demandedSources(), 4U);
  bindings.beginUpdate();
  // New owners sort before old removals: acquisition order cannot steal their
  // capacity or leave the new output permanently dormant after the old release.
  for (int i = 4; i < 8; ++i) {
    bindings.update(&owners[i], &output, definitions[i].name, true);
    CHECK(bindings.inspect(definitions[i].name) == nullptr);
  }
  for (int i = 0; i < 4; ++i) {
    bindings.remove(&owners[i]);
  }
  CHECK_EQ(bindings.demandedSources(), 0U);
  bindings.endUpdate();
  CHECK_EQ(bindings.demandedSources(), 4U);
  for (int i = 4; i < 8; ++i) {
    CHECK(waitForInput(loop, bindings, definitions[i].name));
  }
}

int main() {
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  sigaction(SIGCHLD, &ignore, nullptr);
  return RUN_TESTS();
}
