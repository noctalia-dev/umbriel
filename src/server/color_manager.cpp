#include "server/color_manager.h"

#include "color-management-v1-server-protocol.h"
#include "output/output.h"
#include "server/color_description.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
    constexpr uint32_t kProtocolVersion = WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION;
#else
    constexpr uint32_t kProtocolVersion = 2;
#endif
    constexpr uint32_t kDefaultSdrWhite = 203;

    float decodeCoordinate(int32_t value) { return static_cast<float>(value) / 1'000'000.0F; }
    int32_t encodeCoordinate(float value) { return static_cast<int32_t>(std::lround(value * 1'000'000.0F)); }

    bool supportedPrimaries(const wlr_color_primaries& primaries) {
      const auto& white = primaries.white;
      if (white.x <= 0.0F || white.y <= 0.0F || white.x + white.y >= 1.0F) {
        return false;
      }
      std::array<std::array<float, 3>, 3> xyz{};
      const std::array points{primaries.red, primaries.green, primaries.blue};
      for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].y == 0.0F) {
          return false;
        }
        xyz[i] = {points[i].x / points[i].y, 1.0F, (1.0F - points[i].x - points[i].y) / points[i].y};
      }
      // Validate the same float XYZ basis the renderer inverts. Reject poorly
      // conditioned bases before an untrusted description can reach its assert.
      const auto& r = xyz[0];
      const auto& g = xyz[1];
      const auto& b = xyz[2];
      const std::array terms{r[0] * g[1] * b[2], g[0] * b[1] * r[2], b[0] * r[1] * g[2],
                             b[0] * g[1] * r[2], g[0] * r[1] * b[2], r[0] * b[1] * g[2]};
      const float determinant = terms[0] + terms[1] + terms[2] - terms[3] - terms[4] - terms[5];
      float magnitude = 0.0F;
      for (const float term : terms) {
        magnitude += std::abs(term);
      }
      if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001F * magnitude) {
        return false;
      }
      const float x = white.x / white.y;
      const float z = (1.0F - white.x - white.y) / white.y;
      // Bradford adaptation divides by these source cone responses.
      return 0.8951F * x + 0.2664F - 0.1614F * z > 0.000001F
          && -0.7502F * x + 1.7135F + 0.0367F * z > 0.000001F
          && 0.0389F * x - 0.0685F + 1.0296F * z > 0.000001F;
    }

  } // namespace

  struct ColorManager::Impl {
    struct ImageDescription {
      wl_resource* resource = nullptr;
      ColorDescription description;
      bool informationAllowed = false;
    };

    struct ParametricCreator {
      Impl* manager = nullptr;
      wl_resource* resource = nullptr;
      ColorDescription description;
      bool transferFunctionSet = false;
      bool primariesSet = false;
    };

    struct SurfaceState {
      ColorDescription description;
      bool set = false;
    };

    struct Surface {
      Impl* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      wlr_surface_synced synced{};
      SurfaceState pending;
      SurfaceState current;
      SurfaceState applied;
      wl_listener commit{};
      wl_listener map{};
      wl_listener unmap{};
      wl_listener destroy{};
    };

    struct SurfaceFeedback {
      Impl* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      ColorDescription preferred;
      uint64_t identity = 0;
      wl_listener destroy{};
    };

    struct ManagedOutput {
      Impl* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_output* output = nullptr;
      ColorDescription description;
      wl_listener commit{};
      wl_listener destroy{};
    };

    struct ManagedScene {
      Impl* manager = nullptr;
      wlr_scene* scene = nullptr;
      wl_listener destroy{};
    };

    explicit Impl(Server& server_) : server(server_) {
      size_t transferFunctionCount = 0;
      wp_color_manager_v1_transfer_function* rendererTransferFunctions =
          wlr_color_manager_v1_transfer_function_list_from_renderer(server.renderer(), &transferFunctionCount);
      if (rendererTransferFunctions != nullptr) {
        transferFunctions.assign(rendererTransferFunctions, rendererTransferFunctions + transferFunctionCount);
        std::free(rendererTransferFunctions);
      }

      size_t primariesCount = 0;
      wp_color_manager_v1_primaries* rendererPrimaries =
          wlr_color_manager_v1_primaries_list_from_renderer(server.renderer(), &primariesCount);
      if (rendererPrimaries != nullptr) {
        primaries.assign(rendererPrimaries, rendererPrimaries + primariesCount);
        std::free(rendererPrimaries);
      }

      const bool hasPq = contains(transferFunctions, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
      const bool hasExtendedLinear = contains(transferFunctions, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
      const bool hasSrgb = contains(primaries, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
      const bool hasBt2020 = contains(primaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
      if (!hasPq || !hasExtendedLinear || !hasSrgb || !hasBt2020) {
        return;
      }

      if (!registerScene(server.scene())) {
        return;
      }
      global = wl_global_create(server.display(), &wp_color_manager_v1_interface, kProtocolVersion, this, handleBind);
    }

    ~Impl() {
      while (!scenes.empty()) {
        unregisterScene(scenes.begin()->first);
      }
      if (global != nullptr) {
        wl_global_destroy(global);
        global = nullptr;
      }
      while (!outputs.empty()) {
        auto* output = *outputs.begin();
        destroyManagedOutput(output, true);
      }
      while (!surfaces.empty()) {
        destroySurface(surfaces.begin()->second, true);
      }
      while (!feedbacks.empty()) {
        auto* feedback = *feedbacks.begin();
        destroySurfaceFeedback(feedback, true);
      }
    }

    bool registerScene(wlr_scene* scene) {
      if (scenes.contains(scene)) {
        return true;
      }
      auto managed = std::make_unique<ManagedScene>();
      managed->manager = this;
      managed->scene = scene;
      if (!wlr_scene_set_surface_color_resolver(scene, resolveSurfaceColor, this)) {
        return false;
      }
      managed->destroy.notify = handleSceneDestroy;
      wl_signal_add(&scene->tree.node.events.destroy, &managed->destroy);
      scenes.emplace(scene, std::move(managed));
      return true;
    }

    void unregisterScene(wlr_scene* scene) {
      const auto found = scenes.find(scene);
      if (found == scenes.end()) {
        return;
      }
      wl_list_remove(&found->second->destroy.link);
      wlr_scene_set_surface_color_resolver(scene, nullptr, nullptr);
      scenes.erase(found);
    }

    static void handleSceneDestroy(wl_listener* listener, void*) {
      ManagedScene* scene;
      scene = wl_container_of(listener, scene, destroy);
      scene->manager->unregisterScene(scene->scene);
    }

    template <typename Value> static bool contains(const std::vector<Value>& values, Value value) {
      return std::ranges::find(values, value) != values.end();
    }

    static const struct wp_color_manager_v1_interface& managerImplementation() {
      static const struct wp_color_manager_v1_interface implementation = {
          .destroy = handleResourceDestroy,
          .get_output = handleGetOutput,
          .get_surface = handleGetSurface,
          .get_surface_feedback = handleGetSurfaceFeedback,
          .create_icc_creator = handleCreateIccCreator,
          .create_parametric_creator = handleCreateParametricCreator,
          .create_windows_scrgb = handleCreateWindowsScrgb,
          .get_image_description = handleGetReferencedImageDescription,
#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
          .create_windows_bt2100 = handleCreateWindowsBt2100,
#endif
      };
      return implementation;
    }

    static const struct wp_color_management_output_v1_interface& outputImplementation() {
      static const struct wp_color_management_output_v1_interface implementation = {
          .destroy = handleResourceDestroy,
          .get_image_description = handleOutputGetImageDescription,
      };
      return implementation;
    }

    static const struct wp_color_management_surface_v1_interface& surfaceImplementation() {
      static const struct wp_color_management_surface_v1_interface implementation = {
          .destroy = handleResourceDestroy,
          .set_image_description = handleSurfaceSetImageDescription,
          .unset_image_description = handleSurfaceUnsetImageDescription,
      };
      return implementation;
    }

    static const struct wp_color_management_surface_feedback_v1_interface& feedbackImplementation() {
      static const struct wp_color_management_surface_feedback_v1_interface implementation = {
          .destroy = handleResourceDestroy,
          .get_preferred = handleFeedbackGetPreferred,
          .get_preferred_parametric = handleFeedbackGetPreferred,
      };
      return implementation;
    }

    static const struct wp_image_description_creator_params_v1_interface& parametricCreatorImplementation() {
      static const struct wp_image_description_creator_params_v1_interface implementation = {
          .create = handleParametricCreate,
          .set_tf_named = handleParametricSetTransferFunction,
          .set_tf_power = handleParametricSetTransferPower,
          .set_primaries_named = handleParametricSetPrimaries,
          .set_primaries = handleParametricSetRawPrimaries,
          .set_luminances = handleParametricSetLuminances,
          .set_mastering_display_primaries = handleParametricSetMasteringPrimaries,
          .set_mastering_luminance = handleParametricSetMasteringLuminance,
          .set_max_cll = handleParametricSetMaxCll,
          .set_max_fall = handleParametricSetMaxFall,
      };
      return implementation;
    }

    static const struct wp_image_description_v1_interface& imageDescriptionImplementation() {
      static const struct wp_image_description_v1_interface implementation = {
          .destroy = handleResourceDestroy,
          .get_information = handleImageDescriptionGetInformation,
      };
      return implementation;
    }

    static void handleResourceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static void handleBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
      auto* manager = static_cast<Impl*>(data);
      wl_resource* resource =
          wl_resource_create(client, &wp_color_manager_v1_interface, std::min(version, kProtocolVersion), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &managerImplementation(), manager, nullptr);

      wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
      wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);
      wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES);
      wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES);
      wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME);
      wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB);
#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
      if (wl_resource_get_version(resource) >= WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION) {
        wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100);
      }
#endif
      wp_color_manager_v1_send_supported_intent(resource, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
      for (const wp_color_manager_v1_transfer_function transferFunction : manager->transferFunctions) {
        if (wp_color_manager_v1_transfer_function_is_valid(transferFunction, wl_resource_get_version(resource))) {
          wp_color_manager_v1_send_supported_tf_named(resource, transferFunction);
        }
      }
      for (const wp_color_manager_v1_primaries namedPrimaries : manager->primaries) {
        wp_color_manager_v1_send_supported_primaries_named(resource, namedPrimaries);
      }
      wp_color_manager_v1_send_done(resource);
    }

    static void
    handleGetOutput(wl_client* client, wl_resource* managerResource, uint32_t id, wl_resource* outputResource) {
      auto* manager = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      wl_resource* resource = wl_resource_create(
          client, &wp_color_management_output_v1_interface, wl_resource_get_version(managerResource), id
      );
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &outputImplementation(), nullptr, handleOutputResourceDestroy);

      wlr_output* output = wlr_output_from_resource(outputResource);
      if (output == nullptr) {
        return;
      }

      auto* managed = new ManagedOutput{
          .manager = manager,
          .resource = resource,
          .output = output,
          .description = manager->descriptionForOutput(output),
      };
      managed->commit.notify = handleOutputCommit;
      wl_signal_add(&output->events.commit, &managed->commit);
      managed->destroy.notify = handleWlrOutputDestroy;
      wl_signal_add(&output->events.destroy, &managed->destroy);
      manager->outputs.insert(managed);
      wl_resource_set_user_data(resource, managed);
    }

    static void handleOutputGetImageDescription(wl_client*, wl_resource* resource, uint32_t id) {
      auto* output = static_cast<ManagedOutput*>(wl_resource_get_user_data(resource));
      if (output == nullptr) {
        createFailedImageDescription(resource, id, WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT, "output was destroyed");
        return;
      }
      output->manager->createReadyImageDescription(
          resource, id, output->manager->descriptionForOutput(output->output), true
      );
    }

    static void handleOutputCommit(wl_listener* listener, void* data) {
      ManagedOutput* output;
      output = wl_container_of(listener, output, commit);
      (void)data;
      output->manager->updateOutputDescription(output);
    }

    static void handleWlrOutputDestroy(wl_listener* listener, void*) {
      ManagedOutput* output;
      output = wl_container_of(listener, output, destroy);
      destroyManagedOutput(output, true);
    }

    static void handleOutputResourceDestroy(wl_resource* resource) {
      destroyManagedOutput(static_cast<ManagedOutput*>(wl_resource_get_user_data(resource)), false);
    }

    static void destroyManagedOutput(ManagedOutput* output, bool makeInert) {
      if (output == nullptr) {
        return;
      }
      if (makeInert) {
        wl_resource_set_user_data(output->resource, nullptr);
      }
      wl_list_remove(&output->commit.link);
      wl_list_remove(&output->destroy.link);
      output->manager->outputs.erase(output);
      delete output;
    }

    static void
    handleGetSurface(wl_client* client, wl_resource* managerResource, uint32_t id, wl_resource* surfaceResource) {
      auto* manager = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
      if (manager->surfaces.contains(surface)) {
        wl_resource_post_error(
            managerResource, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
            "wp_color_management_surface_v1 already exists for this surface"
        );
        return;
      }

      auto* managed = new Surface{};
      managed->manager = manager;
      managed->surface = surface;
      static const wlr_surface_synced_impl syncedImplementation{
          .state_size = sizeof(SurfaceState),
          .init_state = [](void* state) { *static_cast<SurfaceState*>(state) = {}; },
          .finish_state = nullptr,
          .move_state = nullptr,
          .commit = nullptr,
      };
      if (!wlr_surface_synced_init(
              &managed->synced, surface, &syncedImplementation, &managed->pending, &managed->current
          )) {
        delete managed;
        wl_client_post_no_memory(client);
        return;
      }
      managed->resource = wl_resource_create(
          client, &wp_color_management_surface_v1_interface, wl_resource_get_version(managerResource), id
      );
      if (managed->resource == nullptr) {
        wlr_surface_synced_finish(&managed->synced);
        delete managed;
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(
          managed->resource, &surfaceImplementation(), managed, handleSurfaceResourceDestroy
      );
      managed->commit.notify = handleSurfaceCommit;
      wl_signal_add(&surface->events.commit, &managed->commit);
      managed->map.notify = handleSurfaceMap;
      wl_signal_add(&surface->events.map, &managed->map);
      managed->unmap.notify = handleSurfaceUnmap;
      wl_signal_add(&surface->events.unmap, &managed->unmap);
      managed->destroy.notify = handleWlrSurfaceDestroy;
      wl_signal_add(&surface->events.destroy, &managed->destroy);
      manager->surfaces.emplace(surface, managed);
    }

    static void handleSurfaceSetImageDescription(
        wl_client*, wl_resource* resource, wl_resource* imageDescriptionResource, uint32_t renderIntent
    ) {
      auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
      if (surface == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, "surface is inert");
        return;
      }
      if (renderIntent != WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
        wl_resource_post_error(
            resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT, "unsupported render intent"
        );
        return;
      }
      if (!wl_resource_instance_of(
              imageDescriptionResource, &wp_image_description_v1_interface, &imageDescriptionImplementation()
          )) {
        wl_resource_post_error(
            resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION, "invalid image description"
        );
        return;
      }
      auto* imageDescription = static_cast<ImageDescription*>(wl_resource_get_user_data(imageDescriptionResource));
      if (imageDescription == nullptr) {
        wl_resource_post_error(
            resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION, "image description is not ready"
        );
        return;
      }
      surface->pending.description = imageDescription->description;
      surface->pending.set = true;
    }

    static void handleSurfaceUnsetImageDescription(wl_client*, wl_resource* resource) {
      auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
      if (surface == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, "surface is inert");
        return;
      }
      surface->pending.set = false;
    }

    static void handleSurfaceCommit(wl_listener* listener, void*) {
      Surface* surface;
      surface = wl_container_of(listener, surface, commit);
      const bool changed = surface->current.set != surface->applied.set
          || (surface->current.set
              && !colorDescriptionsEqual(surface->current.description, surface->applied.description));
      surface->applied = surface->current;
      if (changed) {
        surface->manager->refreshSurfaceHdr(surface->surface);
      }
    }

    static void handleSurfaceMap(wl_listener* listener, void*) {
      Surface* surface;
      surface = wl_container_of(listener, surface, map);
      surface->manager->refreshSurfaceHdr(surface->surface);
    }

    static void handleSurfaceUnmap(wl_listener* listener, void*) {
      Surface* surface;
      surface = wl_container_of(listener, surface, unmap);
      surface->manager->refreshSurfaceHdr(surface->surface);
    }

    static void handleWlrSurfaceDestroy(wl_listener* listener, void*) {
      Surface* surface;
      surface = wl_container_of(listener, surface, destroy);
      destroySurface(surface, true);
    }

    static void handleSurfaceResourceDestroy(wl_resource* resource) {
      auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
      destroySurface(surface, false);
    }

    static void destroySurface(Surface* surface, bool destroying) {
      if (surface == nullptr) {
        return;
      }
      wl_resource_set_user_data(surface->resource, nullptr);
      wl_list_remove(&surface->commit.link);
      wl_list_remove(&surface->map.link);
      wl_list_remove(&surface->unmap.link);
      wl_list_remove(&surface->destroy.link);
      wlr_surface_synced_finish(&surface->synced);
      surface->manager->surfaces.erase(surface->surface);
      if (!destroying) {
        for (const auto& entry : surface->manager->scenes) {
          surface->manager->refreshSurfaceColor(&entry.first->tree.node, surface->surface);
        }
        surface->manager->refreshSurfaceHdr(surface->surface);
      }
      delete surface;
    }

    static void handleGetSurfaceFeedback(
        wl_client* client, wl_resource* managerResource, uint32_t id, wl_resource* surfaceResource
    ) {
      auto* manager = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      auto* feedback = new SurfaceFeedback{};
      feedback->manager = manager;
      feedback->surface = wlr_surface_from_resource(surfaceResource);
      feedback->resource = wl_resource_create(
          client, &wp_color_management_surface_feedback_v1_interface, wl_resource_get_version(managerResource), id
      );
      if (feedback->resource == nullptr) {
        delete feedback;
        wl_client_post_no_memory(client);
        return;
      }
      feedback->preferred = manager->preferredDescription(feedback->surface);
      feedback->identity = ++manager->lastIdentity;
      feedback->destroy.notify = handleFeedbackSurfaceDestroy;
      wl_signal_add(&feedback->surface->events.destroy, &feedback->destroy);
      wl_resource_set_implementation(
          feedback->resource, &feedbackImplementation(), feedback, handleFeedbackResourceDestroy
      );
      manager->feedbacks.insert(feedback);
    }

    static void handleFeedbackGetPreferred(wl_client*, wl_resource* resource, uint32_t id) {
      auto* feedback = static_cast<SurfaceFeedback*>(wl_resource_get_user_data(resource));
      if (feedback == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT, "surface is inert");
        return;
      }
      feedback->manager->createReadyImageDescription(resource, id, feedback->preferred, true, feedback->identity);
    }

    static void handleFeedbackSurfaceDestroy(wl_listener* listener, void*) {
      SurfaceFeedback* feedback;
      feedback = wl_container_of(listener, feedback, destroy);
      destroySurfaceFeedback(feedback, true);
    }

    static void handleFeedbackResourceDestroy(wl_resource* resource) {
      destroySurfaceFeedback(static_cast<SurfaceFeedback*>(wl_resource_get_user_data(resource)), false);
    }

    static void destroySurfaceFeedback(SurfaceFeedback* feedback, bool makeInert) {
      if (feedback == nullptr) {
        return;
      }
      if (makeInert) {
        wl_resource_set_user_data(feedback->resource, nullptr);
      }
      wl_list_remove(&feedback->destroy.link);
      feedback->manager->feedbacks.erase(feedback);
      delete feedback;
    }

    static void handleCreateIccCreator(wl_client*, wl_resource* resource, uint32_t) {
      wl_resource_post_error(resource, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, "ICC profiles are unsupported");
    }

    static void handleCreateParametricCreator(wl_client* client, wl_resource* managerResource, uint32_t id) {
      auto* manager = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      auto* creator = new ParametricCreator{};
      creator->manager = manager;
      creator->resource = wl_resource_create(
          client, &wp_image_description_creator_params_v1_interface, wl_resource_get_version(managerResource), id
      );
      if (creator->resource == nullptr) {
        delete creator;
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(
          creator->resource, &parametricCreatorImplementation(), creator, handleParametricCreatorResourceDestroy
      );
    }

    static void handleParametricCreate(wl_client*, wl_resource* resource, uint32_t id) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (!creator->transferFunctionSet || !creator->primariesSet) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
            "transfer function and primaries are required"
        );
        return;
      }
      if (creator->description.data.max_cll != 0
          && creator->description.data.max_fall > creator->description.data.max_cll) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, "MaxFALL must not exceed MaxCLL"
        );
        return;
      }
      if (creator->description.data.primaries_named == 0 && !supportedPrimaries(creator->description.customPrimaries)) {
        creator->manager->createFailedImageDescription(
            resource, id, WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED, "unsupported color primaries"
        );
        wl_resource_destroy(resource);
        return;
      }
      if (creator->description.customLuminances
          && creator->description.data.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
        creator->description.luminances.max = creator->description.luminances.min + 10000.0F;
      }
      creator->description.requiresHdrOutput =
          creator->description.data.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
          || creator->description.data.mastering_luminance.max > creator->description.luminances.reference
          || creator->description.luminances.max > creator->description.luminances.reference;
      creator->manager->createReadyImageDescription(resource, id, creator->description, false);
      wl_resource_destroy(resource);
    }

    static void handleParametricSetTransferFunction(wl_client*, wl_resource* resource, uint32_t transferFunction) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (creator->transferFunctionSet) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "transfer function is already set"
        );
        return;
      }
      if (!wp_color_manager_v1_transfer_function_is_valid(transferFunction, wl_resource_get_version(resource))
          || !contains(
              creator->manager->transferFunctions, static_cast<wp_color_manager_v1_transfer_function>(transferFunction)
          )) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF, "unsupported transfer function"
        );
        return;
      }
      creator->description.data.tf_named = transferFunction;
      if (!creator->description.customLuminances) {
        creator->description.luminances = defaultColorLuminances(transferFunction);
      }
      creator->transferFunctionSet = true;
    }

    static void handleParametricSetTransferPower(wl_client*, wl_resource* resource, uint32_t) {
      wl_resource_post_error(
          resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
          "power transfer functions are unsupported"
      );
    }

    static void handleParametricSetPrimaries(wl_client*, wl_resource* resource, uint32_t namedPrimaries) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (creator->primariesSet) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "primaries are already set"
        );
        return;
      }
      if (!contains(creator->manager->primaries, static_cast<wp_color_manager_v1_primaries>(namedPrimaries))) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
            "unsupported named primaries"
        );
        return;
      }
      creator->description.data.primaries_named = namedPrimaries;
      creator->primariesSet = true;
    }

    static void handleParametricSetRawPrimaries(
        wl_client*, wl_resource* resource, int32_t redX, int32_t redY, int32_t greenX, int32_t greenY, int32_t blueX,
        int32_t blueY, int32_t whiteX, int32_t whiteY
    ) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (creator->primariesSet) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "primaries are already set"
        );
        return;
      }
      creator->description.customPrimaries = {
          .red = {decodeCoordinate(redX), decodeCoordinate(redY)},
          .green = {decodeCoordinate(greenX), decodeCoordinate(greenY)},
          .blue = {decodeCoordinate(blueX), decodeCoordinate(blueY)},
          .white = {decodeCoordinate(whiteX), decodeCoordinate(whiteY)},
      };
      creator->primariesSet = true;
    }

    static void handleParametricSetLuminances(
        wl_client*, wl_resource* resource, uint32_t minimum, uint32_t maximum, uint32_t reference
    ) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (creator->description.customLuminances) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "luminances are already set"
        );
        return;
      }
      const float min = static_cast<float>(minimum) / 10000.0F;
      if (static_cast<float>(maximum) <= min || static_cast<float>(reference) <= min) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
            "maximum and reference luminance must exceed minimum luminance"
        );
        return;
      }
      creator->description.luminances = {
          .min = min,
          .max = static_cast<float>(maximum),
          .reference = static_cast<float>(reference),
      };
      creator->description.customLuminances = true;
    }

    static void handleParametricSetMasteringPrimaries(
        wl_client*, wl_resource* resource, int32_t redX, int32_t redY, int32_t greenX, int32_t greenY, int32_t blueX,
        int32_t blueY, int32_t whiteX, int32_t whiteY
    ) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (creator->description.data.has_mastering_display_primaries) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
            "mastering display primaries are already set"
        );
        return;
      }
      creator->description.data.has_mastering_display_primaries = true;
      creator->description.data.mastering_display_primaries = {
          .red = {decodeCoordinate(redX), decodeCoordinate(redY)},
          .green = {decodeCoordinate(greenX), decodeCoordinate(greenY)},
          .blue = {decodeCoordinate(blueX), decodeCoordinate(blueY)},
          .white = {decodeCoordinate(whiteX), decodeCoordinate(whiteY)},
      };
    }

    static void
    handleParametricSetMasteringLuminance(wl_client*, wl_resource* resource, uint32_t minimum, uint32_t maximum) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      if (creator->description.data.has_mastering_luminance) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "mastering luminance is already set"
        );
        return;
      }
      const float decodedMinimum = static_cast<float>(minimum) / 10000.0F;
      if (static_cast<float>(maximum) <= decodedMinimum) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
            "maximum mastering luminance must exceed minimum mastering luminance"
        );
        return;
      }
      creator->description.data.has_mastering_luminance = true;
      creator->description.data.mastering_luminance = {
          .min = decodedMinimum,
          .max = static_cast<float>(maximum),
      };
    }

    static void handleParametricSetMaxCll(wl_client*, wl_resource* resource, uint32_t maxCll) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      creator->description.data.max_cll = maxCll;
    }

    static void handleParametricSetMaxFall(wl_client*, wl_resource* resource, uint32_t maxFall) {
      auto* creator = static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
      creator->description.data.max_fall = maxFall;
    }

    static void handleParametricCreatorResourceDestroy(wl_resource* resource) {
      delete static_cast<ParametricCreator*>(wl_resource_get_user_data(resource));
    }

    static void handleCreateWindowsScrgb(wl_client*, wl_resource* managerResource, uint32_t id) {
      auto* manager = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      ColorDescription description;
      description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
      description.data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
      description.luminances = {.min = 0.0F, .max = 10000.0F, .reference = 203.0F};
      description.luminanceMultiplier = 80.0F / 203.0F;
      description.absoluteLuminance = true;
      description.requiresHdrOutput = true;
      manager->createReadyImageDescription(managerResource, id, description, false);
    }

#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
    static void handleCreateWindowsBt2100(wl_client*, wl_resource* managerResource, uint32_t id) {
      auto* manager = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      ColorDescription description;
      description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
      description.data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
      description.luminances = defaultColorLuminances(description.data.tf_named);
      description.requiresHdrOutput = true;
      manager->createReadyImageDescription(managerResource, id, description, false);
    }
#endif

    static void handleGetReferencedImageDescription(wl_client*, wl_resource* resource, uint32_t, wl_resource*) {
      wl_resource_post_error(
          resource, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, "referenced image descriptions are unsupported"
      );
    }

    void createReadyImageDescription(
        wl_resource* parent, uint32_t id, const ColorDescription& description, bool informationAllowed,
        uint64_t identity = 0
    ) {
      if (!wp_color_manager_v1_transfer_function_is_valid(description.data.tf_named, wl_resource_get_version(parent))) {
        createFailedImageDescription(
            parent, id, WP_IMAGE_DESCRIPTION_V1_CAUSE_LOW_VERSION, "transfer function requires a newer protocol version"
        );
        return;
      }
      auto* imageDescription = new ImageDescription{
          .description = description,
          .informationAllowed = informationAllowed,
      };
      imageDescription->resource = wl_resource_create(
          wl_resource_get_client(parent), &wp_image_description_v1_interface, wl_resource_get_version(parent), id
      );
      if (imageDescription->resource == nullptr) {
        delete imageDescription;
        wl_resource_post_no_memory(parent);
        return;
      }
      wl_resource_set_implementation(
          imageDescription->resource, &imageDescriptionImplementation(), imageDescription,
          handleImageDescriptionResourceDestroy
      );
      sendReady(imageDescription->resource, identity != 0 ? identity : ++lastIdentity);
    }

    static void createFailedImageDescription(wl_resource* parent, uint32_t id, uint32_t cause, const char* message) {
      wl_resource* resource = wl_resource_create(
          wl_resource_get_client(parent), &wp_image_description_v1_interface, wl_resource_get_version(parent), id
      );
      if (resource == nullptr) {
        wl_resource_post_no_memory(parent);
        return;
      }
      wl_resource_set_implementation(resource, &imageDescriptionImplementation(), nullptr, nullptr);
      wp_image_description_v1_send_failed(resource, cause, message);
    }

    static void sendReady(wl_resource* resource, uint64_t identity) {
      if (wl_resource_get_version(resource) >= WP_IMAGE_DESCRIPTION_V1_READY2_SINCE_VERSION) {
        wp_image_description_v1_send_ready2(
            resource, static_cast<uint32_t>(identity >> 32), static_cast<uint32_t>(identity)
        );
      } else {
        wp_image_description_v1_send_ready(resource, static_cast<uint32_t>(identity));
      }
    }

    static void handleImageDescriptionGetInformation(wl_client* client, wl_resource* resource, uint32_t id) {
      auto* imageDescription = static_cast<ImageDescription*>(wl_resource_get_user_data(resource));
      if (imageDescription == nullptr) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY, "image description is not ready");
        return;
      }
      if (!imageDescription->informationAllowed) {
        wl_resource_post_error(
            resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION, "image description information is unavailable"
        );
        return;
      }

      wl_resource* information =
          wl_resource_create(client, &wp_image_description_info_v1_interface, wl_resource_get_version(resource), id);
      if (information == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }

      const ColorDescription& description = imageDescription->description;
      wlr_color_primaries primaryCoordinates = description.customPrimaries;
      if (description.data.primaries_named != 0) {
        wlr_color_primaries_from_named(
            &primaryCoordinates,
            wlr_color_manager_v1_primaries_to_wlr(
                static_cast<wp_color_manager_v1_primaries>(description.data.primaries_named)
            )
        );
        wp_image_description_info_v1_send_primaries_named(information, description.data.primaries_named);
      }
      sendPrimaries(information, primaryCoordinates);
      wp_image_description_info_v1_send_tf_named(information, description.data.tf_named);
      wp_image_description_info_v1_send_luminances(
          information, static_cast<uint32_t>(std::lround(description.luminances.min * 10000.0F)),
          static_cast<uint32_t>(std::lround(description.luminances.max)),
          static_cast<uint32_t>(std::lround(description.luminances.reference))
      );
      sendTargetPrimaries(
          information,
          description.data.has_mastering_display_primaries ? description.data.mastering_display_primaries
                                                           : primaryCoordinates
      );
      wp_image_description_info_v1_send_target_luminance(
          information,
          static_cast<uint32_t>(std::lround(
              (description.data.has_mastering_luminance ? description.data.mastering_luminance.min
                                                        : description.luminances.min)
              * 10000.0F
          )),
          static_cast<uint32_t>(std::lround(
              description.data.has_mastering_luminance ? description.data.mastering_luminance.max
                                                       : description.luminances.max
          ))
      );
      if (description.data.max_cll != 0) {
        wp_image_description_info_v1_send_target_max_cll(information, description.data.max_cll);
      }
      if (description.data.max_fall != 0) {
        wp_image_description_info_v1_send_target_max_fall(information, description.data.max_fall);
      }
      wp_image_description_info_v1_send_done(information);
      wl_resource_destroy(information);
    }

    static void sendPrimaries(wl_resource* information, const wlr_color_primaries& primaries) {
      wp_image_description_info_v1_send_primaries(
          information, encodeCoordinate(primaries.red.x), encodeCoordinate(primaries.red.y),
          encodeCoordinate(primaries.green.x), encodeCoordinate(primaries.green.y), encodeCoordinate(primaries.blue.x),
          encodeCoordinate(primaries.blue.y), encodeCoordinate(primaries.white.x), encodeCoordinate(primaries.white.y)
      );
    }

    static void sendTargetPrimaries(wl_resource* information, const wlr_color_primaries& primaries) {
      wp_image_description_info_v1_send_target_primaries(
          information, encodeCoordinate(primaries.red.x), encodeCoordinate(primaries.red.y),
          encodeCoordinate(primaries.green.x), encodeCoordinate(primaries.green.y), encodeCoordinate(primaries.blue.x),
          encodeCoordinate(primaries.blue.y), encodeCoordinate(primaries.white.x), encodeCoordinate(primaries.white.y)
      );
    }

    static void handleImageDescriptionResourceDestroy(wl_resource* resource) {
      delete static_cast<ImageDescription*>(wl_resource_get_user_data(resource));
    }

    ColorDescription descriptionForOutput(wlr_output* output) const {
      const Output* managedOutput = server.outputFromWlr(output);
      const float sdrWhite = managedOutput != nullptr ? managedOutput->sdrWhite() : kDefaultSdrWhite;
      const bool automaticHdr = managedOutput != nullptr && managedOutput->automaticHdrAvailable();
      return outputColorDescription(output->image_description, sdrWhite, automaticHdr);
    }

    ColorDescription preferredDescription(wlr_surface* surface) const {
      wlr_output* preferred = nullptr;
      if (View* view = View::fromSurface(wlr_surface_get_root_surface(surface)); view != nullptr) {
        if (Output* output = view->currentOutput()) {
          preferred = output->wlr();
        }
      }
      // A surface crossing outputs may need HDR even when its main output is
      // SDR. Include subsurfaces and layer clients through actual membership.
      wlr_output* selected = nullptr;
      wlr_surface_output* membership;
      wl_list_for_each(membership, &surface->current_outputs, link) {
        wlr_output* candidate = membership->output;
        const bool candidateHdr = descriptionForOutput(candidate).requiresHdrOutput;
        if (selected == nullptr
            || (!descriptionForOutput(selected).requiresHdrOutput && candidateHdr)
            || (candidate == preferred && candidateHdr)) {
          selected = candidate;
        }
      }
      if (selected == nullptr) {
        selected = preferred != nullptr ? preferred : server.preferredOutput();
      }
      return selected != nullptr ? descriptionForOutput(selected) : outputColorDescription(nullptr, kDefaultSdrWhite);
    }

    void updateOutputDescription(ManagedOutput* output) {
      ColorDescription description = descriptionForOutput(output->output);
      if (colorDescriptionsEqual(description, output->description)) {
        return;
      }
      output->description = description;
      wp_color_management_output_v1_send_image_description_changed(output->resource);
      wlr_output_schedule_done(output->output);
    }

    void updatePreferredDescriptions() {
      for (ManagedOutput* output : outputs) {
        updateOutputDescription(output);
      }
      for (SurfaceFeedback* feedback : feedbacks) {
        ColorDescription preferred = preferredDescription(feedback->surface);
        if (colorDescriptionsEqual(preferred, feedback->preferred)) {
          continue;
        }
        feedback->preferred = preferred;
        feedback->identity = ++lastIdentity;
        if (wl_resource_get_version(feedback->resource)
            >= WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION) {
          wp_color_management_surface_feedback_v1_send_preferred_changed2(
              feedback->resource, static_cast<uint32_t>(feedback->identity >> 32),
              static_cast<uint32_t>(feedback->identity)
          );
        } else {
          wp_color_management_surface_feedback_v1_send_preferred_changed(
              feedback->resource, static_cast<uint32_t>(feedback->identity)
          );
        }
      }
    }

    void refreshSurfaceHdr(wlr_surface* surface) {
      if (server.stopping() || surface == nullptr) {
        return;
      }
      View* view = View::fromSurface(wlr_surface_get_root_surface(surface));
      if (view == nullptr || !view->mapped()) {
        return;
      }
      if (Output* output = view->currentOutput()) {
        output->updateHdr();
      }
    }

    static void resolveSurfaceColor(wlr_surface* surface, wlr_scene_surface_color_state* state, void* data) {
      const auto* manager = static_cast<Impl*>(data);
      const auto found = manager->surfaces.find(surface);
      if (found == manager->surfaces.end() || !found->second->current.set) {
        return;
      }
      const ColorDescription& description = found->second->current.description;
      state->transfer_function = wlr_color_manager_v1_transfer_function_to_wlr(
          static_cast<wp_color_manager_v1_transfer_function>(description.data.tf_named)
      );
      state->primaries = description.data.primaries_named != 0
          ? wlr_color_manager_v1_primaries_to_wlr(
                static_cast<wp_color_manager_v1_primaries>(description.data.primaries_named)
            )
          : WLR_COLOR_NAMED_PRIMARIES_SRGB;
      state->custom_primaries = description.data.primaries_named == 0 ? &description.customPrimaries : nullptr;
      state->luminances = description.customLuminances
          ? wlr_color_luminances{description.luminances.min, description.luminances.max, description.luminances.reference}
          : wlr_color_luminances{};
      state->absolute_luminance = description.absoluteLuminance;
      state->luminance_multiplier = description.luminanceMultiplier;
    }

    // Color-object destruction is immediate and need not be followed by a
    // surface commit. Include disabled trees so revealing a hidden surface
    // cannot revive a description whose protocol object has gone away.
    void refreshSurfaceColor(wlr_scene_node* node, wlr_surface* surface) {
      if (node->type == WLR_SCENE_NODE_BUFFER) {
        wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (sceneSurface != nullptr && sceneSurface->surface == surface) {
          wlr_scene_surface_update_color(sceneSurface);
        }
      } else if (node->type == WLR_SCENE_NODE_TREE) {
        wlr_scene_node* child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
          refreshSurfaceColor(child, surface);
        }
      }
    }

    void applySurfaceDescriptionToBuffer(wlr_surface* surface, wlr_scene_buffer* buffer) const {
      if (surface == nullptr || buffer == nullptr) {
        return;
      }
      const auto found = surfaces.find(surface);
      if (found == surfaces.end()) {
        return;
      }
      wlr_scene_surface_color_state state{
          .transfer_function = static_cast<wlr_color_transfer_function>(0),
          .primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB,
          .luminance_multiplier = 1.0F,
          .custom_primaries = nullptr,
          .luminances = {},
          .absolute_luminance = false,
      };
      resolveSurfaceColor(surface, &state, const_cast<Impl*>(this));
      wlr_scene_buffer_set_transfer_function(buffer, state.transfer_function);
      wlr_scene_buffer_set_primaries(buffer, state.primaries);
      wlr_scene_buffer_set_custom_primaries(buffer, state.custom_primaries);
      wlr_scene_buffer_set_luminances(buffer, &state.luminances, state.absolute_luminance);
      wlr_scene_buffer_set_luminance_multiplier(buffer, state.luminance_multiplier);
    }

    Server& server;
    wl_global* global = nullptr;
    uint64_t lastIdentity = 0;
    std::vector<wp_color_manager_v1_transfer_function> transferFunctions;
    std::vector<wp_color_manager_v1_primaries> primaries;
    std::unordered_map<wlr_surface*, Surface*> surfaces;
    std::unordered_set<SurfaceFeedback*> feedbacks;
    std::unordered_set<ManagedOutput*> outputs;
    std::unordered_map<wlr_scene*, std::unique_ptr<ManagedScene>> scenes;
  };

  ColorManager::ColorManager(Server& server) : m_impl(std::make_unique<Impl>(server)) {}
  ColorManager::~ColorManager() = default;

  bool ColorManager::valid() const { return m_impl->global != nullptr; }
  wl_global* ColorManager::global() const { return m_impl->global; }

  const wlr_image_description_v1_data* ColorManager::surfaceDescription(wlr_surface* surface) const {
    const auto found = m_impl->surfaces.find(surface);
    return found != m_impl->surfaces.end() && found->second->current.set ? &found->second->current.description.data
                                                                         : nullptr;
  }

  bool ColorManager::surfaceRequiresHdrOutput(wlr_surface* surface) const {
    const auto found = m_impl->surfaces.find(surface);
    return found != m_impl->surfaces.end()
        && found->second->current.set
        && colorDescriptionRequestsHdrOutput(found->second->current.description);
  }

  bool ColorManager::registerScene(wlr_scene* scene) { return m_impl->registerScene(scene); }

  void ColorManager::applySurfaceDescriptionToBuffer(wlr_surface* surface, wlr_scene_buffer* buffer) const {
    m_impl->applySurfaceDescriptionToBuffer(surface, buffer);
  }

  void ColorManager::updatePreferredDescriptions() { m_impl->updatePreferredDescriptions(); }

} // namespace umbriel
