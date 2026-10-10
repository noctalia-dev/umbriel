// Live reveal composition: native blur backdrop and atomic capture failure.
static bool
reveal_pixel(struct fixture* fixture, struct wlr_scene_output* output, int x, int r, int g, int b, const char* label) {
  struct wlr_output_state state;
  struct wlr_buffer* frame = fixture_render_scene(fixture, output, &state);
  bool ok = check(frame != NULL, label);
  if (frame != NULL) {
    uint8_t pixel[4];
    bool read = fixture_read_pixel(fixture, frame, x, 8, pixel);
    ok &= read;
    if (read) {
      fprintf(stderr, "%s: RGB %u %u %u\n", label, pixel[2], pixel[1], pixel[0]);
      ok &= check(abs((int)pixel[2] - r) <= 3 && abs((int)pixel[1] - g) <= 3 && abs((int)pixel[0] - b) <= 3, label);
    }
    wlr_buffer_unlock(frame);
  }
  wlr_output_state_finish(&state);
  return ok;
}

struct reveal_sample_counter {
  struct wl_listener listener;
  struct wlr_renderer* renderer;
  unsigned count;
  bool fail_after_sample;
};

static void reveal_sample(struct wl_listener* listener, void* data) {
  (void)data;
  struct reveal_sample_counter* counter = wl_container_of(listener, counter, listener);
  counter->count++;
  if (counter->fail_after_sample) {
    fx_renderer_fail_animation_capture_for_test(counter->renderer, true);
  }
}

static bool test_workspace_reveal(struct fixture* fixture) {
  const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
  struct fx_effect_shader* wipe = fx_effect_shader_create(
      fixture->renderer, FX_EFFECT_ANIMATION,
      "vec4 animation(vec2 uv) { float a = step(uv.x, umbriel_progress);"
      "if (umbriel_direction < 0.0) a = 1.0-a; return umbriel_sample(uv)*a; }",
      "reveal-wipe"
  );
  if (!check(wipe != NULL, "wipe compiles"))
    return false;
  struct fx_animation_parameters from = {.progress = .5, .direction = -1, .transition_id = 1};
  struct fx_animation_parameters to = {.progress = .5, .direction = 1, .transition_id = 1};
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = wlr_scene_output_create(scene, fixture->output);
  wlr_scene_set_blur_data(scene, 2, 3, 0, 1, 1, 1);
  wlr_scene_rect_create(&scene->tree, 16, 16, green);
  struct wlr_scene_tree* a = wlr_scene_tree_create(&scene->tree);
  wlr_scene_rect_create(a, 16, 16, red);
  struct wlr_buffer* client = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, 16, 16);
  if (!check(client != NULL, "sampled client buffer")) {
    wlr_scene_node_destroy(&scene->tree.node);
    fx_effect_shader_unref(wipe);
    return false;
  }
  struct wlr_render_pass* paint = wlr_renderer_begin_buffer_pass(fixture->renderer, client, NULL);
  if (!check(paint != NULL, "sampled client paint pass")) {
    wlr_buffer_drop(client);
    wlr_scene_node_destroy(&scene->tree.node);
    fx_effect_shader_unref(wipe);
    return false;
  }
  wlr_render_pass_add_rect(
      paint,
      &(struct wlr_render_rect_options){
          .box = {.width = 16, .height = 16}, .color = {.r = 1, .a = 1}, .blend_mode = WLR_RENDER_BLEND_MODE_NONE
      }
  );
  bool painted = wlr_render_pass_submit(paint);
  struct wlr_scene_buffer* sampled = wlr_scene_buffer_create(a, client);
  wlr_buffer_drop(client);
  struct reveal_sample_counter counter = {.listener.notify = reveal_sample, .renderer = fixture->renderer};
  wl_signal_add(&sampled->events.output_sample, &counter.listener);
  struct wlr_scene_tree* b = wlr_scene_tree_create(&scene->tree);
  struct wlr_scene_rect* lower = wlr_scene_rect_create(b, 16, 16, blue);
  wlr_scene_node_set_enabled(&lower->node, false);
  struct wlr_scene_blur* blur = wlr_scene_blur_create(b, 16, 16);
  wlr_scene_blur_set_should_only_blur_bottom_layer(blur, false);
  wlr_scene_node_set_animation(&a->node, FX_SLOT_WORKSPACES, wipe, &from);
  wlr_scene_node_set_animation(&b->node, FX_SLOT_WORKSPACES, wipe, &to);
  wlr_scene_node_set_animation_isolation(&a->node, FX_SLOT_WORKSPACES, a);
  wlr_scene_node_set_animation_isolation(&b->node, FX_SLOT_WORKSPACES, b);
  bool ok = check(painted, "sampled client paint submits");
  ok &= reveal_pixel(fixture, output, 6, 0, 255, 0, "blur retains shared backdrop without outgoing red");
  ok &= reveal_pixel(fixture, output, 12, 255, 0, 0, "outgoing half survives isolation scratch capture");
  wlr_scene_node_set_enabled(&lower->node, true);
  // This client is redrawn into its own fullscreen root's backdrop.
  wlr_scene_node_reparent(&sampled->node, b);
  wlr_scene_node_place_below(&sampled->node, &lower->node);
  ok &= reveal_pixel(fixture, output, 6, 0, 0, 255, "blur retains own lower window");
  // A separate fullscreen root shares its workspace's unmasked normal backdrop.
  struct wlr_scene_tree* fullscreen = wlr_scene_tree_create(&scene->tree);
  struct wlr_scene_blur* full_blur = wlr_scene_blur_create(fullscreen, 16, 16);
  wlr_scene_blur_set_should_only_blur_bottom_layer(full_blur, false);
  wlr_scene_node_set_animation(&fullscreen->node, FX_SLOT_WORKSPACES, wipe, &to);
  wlr_scene_node_set_animation_isolation(&fullscreen->node, FX_SLOT_WORKSPACES, b);
  counter.count = 0;
  ok &= reveal_pixel(fixture, output, 6, 0, 0, 255, "fullscreen blur retains same-workspace normal content");
  ok &= check(counter.count == 1, "backdrop replay emits no duplicate output_sample");
  struct fx_effect_shader* feedback = fx_effect_shader_create(
      fixture->renderer, FX_EFFECT_WINDOW,
      "vec4 window(vec2 uv) { vec4 p = umbriel_sample_previous(uv); return vec4(p.r + 0.0625, 0, 1, 1); }",
      "isolation-feedback"
  );
  ok &= check(feedback != NULL, "nested feedback compiles");
  wlr_scene_node_set_animation(&lower->node, FX_SLOT_WINDOW, feedback, &to);
  ok &= reveal_pixel(fixture, output, 6, 16, 0, 255, "nested feedback advances once despite backdrop replay");
  ok &= reveal_pixel(fixture, output, 6, 32, 0, 255, "nested feedback remains stable on subsequent frame");
  wlr_scene_node_set_animation(&lower->node, FX_SLOT_WINDOW, NULL, NULL);
  fx_effect_shader_unref(feedback);

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  const struct wlr_drm_format* format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
  struct wlr_swapchain* swapchain = format != NULL ? wlr_swapchain_create(fixture->allocator, 16, 16, format) : NULL;
  ok &= check(swapchain != NULL, "strict composition swapchain");
  const struct wlr_scene_output_state_options options = {.require_animation_success = true, .swapchain = swapchain};
  ok &= check(wlr_scene_output_build_state(output, &state, &options), "strict reveal succeeds before injection");
  ok &= check(state.buffer != NULL, "strict success supplies a frame");
  wlr_output_state_finish(&state);
  wlr_output_state_init(&state);
  wlr_scene_output_damage_whole_for_test(output);
  counter.count = 0;
  counter.fail_after_sample = true;
  ok &= check(!wlr_scene_output_build_state(output, &state, &options), "capture failure rejects reveal frame");
  ok &= check(counter.count == 1, "failure occurs after the first participant has rendered");
  counter.fail_after_sample = false;
  ok &= check(!(state.committed & WLR_OUTPUT_STATE_BUFFER), "failed reveal exposes no output buffer");
  wlr_output_state_finish(&state);
  fx_renderer_fail_animation_capture_for_test(fixture->renderer, false);
  wlr_swapchain_destroy(swapchain);
  ok &= reveal_pixel(fixture, output, 6, 0, 0, 255, "capture stack recovers after rejection");
  wl_list_remove(&counter.listener.link);
  wlr_scene_node_destroy(&scene->tree.node);
  fx_effect_shader_unref(wipe);
  return ok;
}

// A tint makes unchanged shared layers observable: drawing them above or below
// the transition would leave them at full intensity instead of half intensity.
static bool test_workspace_pair(struct fixture* fixture) {
  const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};
  const float blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
  struct fx_effect_shader* shader = fx_effect_shader_create(
      fixture->renderer, FX_EFFECT_ANIMATION,
      "vec4 animation(vec2 uv) { return mix(umbriel_sample(uv),"
      "umbriel_sample_incoming(uv), umbriel_progress) * vec4(0.5, 0.5, 0.5, 1.0); }",
      "workspace-pair"
  );
  if (!check(shader != NULL, "pair shader compiles"))
    return false;
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = wlr_scene_output_create(scene, fixture->output);
  wlr_scene_rect_create(&scene->tree, 16, 16, green);
  struct wlr_scene_tree* from = wlr_scene_tree_create(&scene->tree);
  struct wlr_scene_tree* to = wlr_scene_tree_create(&scene->tree);
  struct wlr_scene_rect* outgoing = wlr_scene_rect_create(from, 10, 10, red);
  struct wlr_scene_rect* incoming = wlr_scene_rect_create(to, 10, 10, blue);
  wlr_scene_node_set_position(&outgoing->node, 3, 3);
  wlr_scene_node_set_position(&incoming->node, 3, 3);
  wlr_scene_rect_create(&scene->tree, 2, 16, white); // shared shell surface above both roots
  struct fx_animation_parameters parameters = {.progress = .5, .transition_id = 1};
  wlr_scene_node_set_animation(&from->node, FX_SLOT_WORKSPACES, shader, &parameters);
  wlr_scene_node_set_animation(&to->node, FX_SLOT_WORKSPACES, shader, &parameters);
  wlr_scene_node_set_animation_isolation(&from->node, FX_SLOT_WORKSPACES, from);
  wlr_scene_node_set_animation_isolation(&to->node, FX_SLOT_WORKSPACES, to);
  const struct wlr_drm_format* format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
  struct wlr_swapchain* swapchain = format != NULL ? wlr_swapchain_create(fixture->allocator, 16, 16, format) : NULL;
  bool ok = check(swapchain != NULL, "pair swapchain");
  if (swapchain != NULL) {
    const struct wlr_scene_output_state_options options = {
        .swapchain = swapchain,
        .require_animation_success = true,
        .workspace_from = &from->node,
        .workspace_to = &to->node,
    };
    const int expected[3][3] = {{64, 0, 64}, {0, 64, 64}, {64, 64, 0}};
    for (unsigned update = 0; update < 3; update++) {
      if (update == 1)
        wlr_scene_rect_set_color(outgoing, green);
      if (update == 2)
        wlr_scene_rect_set_color(incoming, red);
      struct wlr_output_state state;
      wlr_output_state_init(&state);
      bool built = wlr_scene_output_build_state(output, &state, &options);
      ok &= check(built && state.buffer != NULL, "live pair renders at held progress");
      if (built && state.buffer != NULL) {
        const int xs[] = {8, 15, 0};
        const int colors[3][3] = {
            {expected[update][0], expected[update][1], expected[update][2]}, {0, 128, 0}, {128, 128, 128}
        };
        for (unsigned sample = 0; sample < 3; sample++) {
          uint8_t pixel[4];
          bool read = fixture_read_pixel(fixture, state.buffer, xs[sample], 8, pixel);
          ok &= check(read, "pair pixel readable");
          if (read)
            ok &= check(
                abs(pixel[2] - colors[sample][0]) <= 3
                    && abs(pixel[1] - colors[sample][1]) <= 3
                    && abs(pixel[0] - colors[sample][2]) <= 3,
                "one shader transforms live windows, wallpaper and shell surface"
            );
        }
      }
      wlr_output_state_finish(&state);
    }
    wlr_swapchain_destroy(swapchain);
  }
  wlr_scene_node_destroy(&scene->tree.node);
  fx_effect_shader_unref(shader);
  return ok;
}
