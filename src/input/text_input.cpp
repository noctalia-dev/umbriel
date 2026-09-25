#include "input/text_input.h"

#include "input/seat.h"
#include "layer/layer_surface.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

  namespace {
    void removeListener(wl_listener& listener) {
      if (listener.link.next == nullptr) {
        return;
      }
      wl_list_remove(&listener.link);
      listener.link.next = nullptr;
      listener.link.prev = nullptr;
    }
  } // namespace

  InputMethodRelay::TextInput::~TextInput() {
    removeListener(enable);
    removeListener(commit);
    removeListener(disable);
    removeListener(destroy);
  }

  InputMethodRelay::Popup::~Popup() {
    removeListener(destroy);
    removeListener(surfaceCommit);
    if (tree != nullptr) {
      wlr_scene_node_destroy(&tree->node);
      tree = nullptr;
    }
  }

  InputMethodRelay::InputMethodRelay(Server& server) : m_server(&server) {
    m_textInputManager = wlr_text_input_manager_v3_create(server.display());
    m_inputMethodManager = wlr_input_method_manager_v2_create(server.display());

    m_newTextInput.notify = onNewTextInput;
    wl_signal_add(&m_textInputManager->events.new_text_input, &m_newTextInput);
    m_newInputMethod.notify = onNewInputMethod;
    wl_signal_add(&m_inputMethodManager->events.new_input_method, &m_newInputMethod);
    m_keyboardFocusChange.notify = onKeyboardFocusChange;
    wl_signal_add(&server.seat()->wlr()->keyboard_state.events.focus_change, &m_keyboardFocusChange);
  }

  InputMethodRelay::~InputMethodRelay() {
    removeListener(m_grabDestroy);
    removeListener(m_inputMethodCommit);
    removeListener(m_inputMethodGrabKeyboard);
    removeListener(m_inputMethodDestroy);
    removeListener(m_inputMethodNewPopup);
    removeListener(m_keyboardFocusChange);
    removeListener(m_newInputMethod);
    removeListener(m_newTextInput);
    m_popups.clear();
    m_textInputs.clear();
  }

  wlr_input_method_keyboard_grab_v2* InputMethodRelay::grabForKeyboard(wlr_keyboard* keyboard) const {
    if (m_inputMethod == nullptr
        || m_inputMethod->keyboard_grab == nullptr
        || keyboard == nullptr
        || ownsKeyboard(keyboard)) {
      return nullptr;
    }
    return m_inputMethod->keyboard_grab;
  }

  bool InputMethodRelay::ownsKeyboard(wlr_keyboard* keyboard) const {
    if (m_inputMethod == nullptr || keyboard == nullptr) {
      return false;
    }
    wlr_virtual_keyboard_v1* virtualKeyboard = wlr_input_device_get_virtual_keyboard(&keyboard->base);
    return virtualKeyboard != nullptr
        && wl_resource_get_client(virtualKeyboard->resource) == wl_resource_get_client(m_inputMethod->resource);
  }

  void InputMethodRelay::canonicalizeOwnedKeyboardKeymap(wlr_keyboard* keyboard) const {
    if (!ownsKeyboard(keyboard) || m_inputMethod->keyboard_grab == nullptr) {
      return;
    }
    wlr_keyboard* grabbedKeyboard = m_inputMethod->keyboard_grab->keyboard;
    if (grabbedKeyboard == nullptr
        || grabbedKeyboard->keymap == nullptr
        || keyboard->keymap == nullptr
        || grabbedKeyboard->keymap == keyboard->keymap
        || !wlr_keyboard_keymaps_match(grabbedKeyboard->keymap, keyboard->keymap)) {
      return;
    }
    // An input method can send the grabbed map back through its virtual
    // keyboard. Share the map object so selecting that mirror is not exposed
    // to every client as a keymap change.
    wlr_keyboard_set_keymap(keyboard, grabbedKeyboard->keymap);
  }

  void InputMethodRelay::onNewTextInput(wl_listener* listener, void* data) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_newTextInput);
    self->handleNewTextInput(static_cast<wlr_text_input_v3*>(data));
  }

  void InputMethodRelay::onTextInputEnable(wl_listener* listener, void* /*data*/) {
    TextInput* textInput;
    textInput = wl_container_of(listener, textInput, enable);
    textInput->relay->updateActiveTextInput();
    wlr_text_input_v3_send_done(textInput->input);
  }

  void InputMethodRelay::onTextInputCommit(wl_listener* listener, void* /*data*/) {
    TextInput* textInput;
    textInput = wl_container_of(listener, textInput, commit);
    if (textInput->relay->m_activeTextInput == textInput->input) {
      textInput->relay->sendStateToInputMethod();
      textInput->relay->updatePopupPositions();
    }
  }

  void InputMethodRelay::onTextInputDisable(wl_listener* listener, void* /*data*/) {
    TextInput* textInput;
    textInput = wl_container_of(listener, textInput, disable);
    textInput->relay->updateActiveTextInput();
  }

  void InputMethodRelay::onTextInputDestroy(wl_listener* listener, void* /*data*/) {
    TextInput* textInput;
    textInput = wl_container_of(listener, textInput, destroy);
    textInput->relay->handleTextInputDestroy(textInput);
  }

  void InputMethodRelay::onNewInputMethod(wl_listener* listener, void* data) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_newInputMethod);
    self->handleNewInputMethod(static_cast<wlr_input_method_v2*>(data));
  }

  void InputMethodRelay::onInputMethodCommit(wl_listener* listener, void* /*data*/) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_inputMethodCommit);
    self->handleInputMethodCommit();
  }

  void InputMethodRelay::onInputMethodGrabKeyboard(wl_listener* listener, void* data) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_inputMethodGrabKeyboard);
    self->handleInputMethodGrabKeyboard(static_cast<wlr_input_method_keyboard_grab_v2*>(data));
  }

  void InputMethodRelay::onInputMethodDestroy(wl_listener* listener, void* /*data*/) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_inputMethodDestroy);
    self->handleInputMethodDestroy();
  }
  void InputMethodRelay::onInputMethodNewPopup(wl_listener* listener, void* data) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_inputMethodNewPopup);
    self->handleInputMethodNewPopup(static_cast<wlr_input_popup_surface_v2*>(data));
  }

  void InputMethodRelay::onGrabDestroy(wl_listener* listener, void* /*data*/) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_grabDestroy);
    self->handleGrabDestroy();
  }
  void InputMethodRelay::onPopupDestroy(wl_listener* listener, void* /*data*/) {
    Popup* popup;
    popup = wl_container_of(listener, popup, destroy);
    popup->relay->handlePopupDestroy(popup);
  }

  void InputMethodRelay::onPopupSurfaceCommit(wl_listener* listener, void* /*data*/) {
    Popup* popup;
    popup = wl_container_of(listener, popup, surfaceCommit);
    popup->relay->updatePopupPosition(*popup);
  }

  void InputMethodRelay::onKeyboardFocusChange(wl_listener* listener, void* data) {
    InputMethodRelay* self;
    self = wl_container_of(listener, self, m_keyboardFocusChange);
    self->handleKeyboardFocusChange(data);
  }

  void InputMethodRelay::handleNewTextInput(wlr_text_input_v3* input) {
    if (input->seat != m_server->seat()->wlr()) {
      return;
    }

    auto textInput = std::make_unique<TextInput>();
    textInput->relay = this;
    textInput->input = input;
    textInput->enable.notify = onTextInputEnable;
    wl_signal_add(&input->events.enable, &textInput->enable);
    textInput->commit.notify = onTextInputCommit;
    wl_signal_add(&input->events.commit, &textInput->commit);
    textInput->disable.notify = onTextInputDisable;
    wl_signal_add(&input->events.disable, &textInput->disable);
    textInput->destroy.notify = onTextInputDestroy;
    wl_signal_add(&input->events.destroy, &textInput->destroy);
    m_textInputs.push_back(std::move(textInput));
    updateTextInputFocus();
  }

  void InputMethodRelay::handleTextInputDestroy(TextInput* input) {
    std::erase_if(m_textInputs, [input](const std::unique_ptr<TextInput>& entry) { return entry.get() == input; });
    updateActiveTextInput();
  }

  void InputMethodRelay::handleNewInputMethod(wlr_input_method_v2* inputMethod) {
    if (inputMethod->seat != m_server->seat()->wlr()) {
      return;
    }
    if (m_inputMethod != nullptr) {
      wlr_input_method_v2_send_unavailable(inputMethod);
      return;
    }

    m_inputMethod = inputMethod;
    m_inputMethodCommit.notify = onInputMethodCommit;
    wl_signal_add(&inputMethod->events.commit, &m_inputMethodCommit);
    m_inputMethodGrabKeyboard.notify = onInputMethodGrabKeyboard;
    wl_signal_add(&inputMethod->events.grab_keyboard, &m_inputMethodGrabKeyboard);
    m_inputMethodDestroy.notify = onInputMethodDestroy;
    wl_signal_add(&inputMethod->events.destroy, &m_inputMethodDestroy);
    m_inputMethodNewPopup.notify = onInputMethodNewPopup;
    wl_signal_add(&inputMethod->events.new_popup_surface, &m_inputMethodNewPopup);
    updateTextInputFocus();
    updateActiveTextInput();
  }

  void InputMethodRelay::handleInputMethodCommit() {
    if (m_activeTextInput == nullptr) {
      return;
    }

    const wlr_input_method_v2_state& current = m_inputMethod->current;
    if (current.preedit.text != nullptr) {
      wlr_text_input_v3_send_preedit_string(
          m_activeTextInput, current.preedit.text, current.preedit.cursor_begin, current.preedit.cursor_end
      );
    }
    if (current.commit_text != nullptr) {
      wlr_text_input_v3_send_commit_string(m_activeTextInput, current.commit_text);
    }
    if (current.delete_.before_length != 0 || current.delete_.after_length != 0) {
      wlr_text_input_v3_send_delete_surrounding_text(
          m_activeTextInput, current.delete_.before_length, current.delete_.after_length
      );
    }
    wlr_text_input_v3_send_done(m_activeTextInput);
  }

  void InputMethodRelay::handleInputMethodGrabKeyboard(wlr_input_method_keyboard_grab_v2* grab) {
    wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_server->seat()->wlr());
    if (keyboard != nullptr && grabForKeyboard(keyboard) == grab) {
      wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
    }
    removeListener(m_grabDestroy);
    m_grabDestroy.notify = onGrabDestroy;
    wl_signal_add(&grab->events.destroy, &m_grabDestroy);
  }

  void InputMethodRelay::handleGrabDestroy() {
    wlr_input_method_keyboard_grab_v2* grab = m_inputMethod != nullptr ? m_inputMethod->keyboard_grab : nullptr;
    removeListener(m_grabDestroy);
    if (grab != nullptr && grab->keyboard != nullptr) {
      wlr_seat* seat = m_server->seat()->wlr();
      wlr_seat_set_keyboard(seat, grab->keyboard);
      wlr_seat_keyboard_notify_modifiers(seat, &grab->keyboard->modifiers);
    }
  }

  void InputMethodRelay::handleInputMethodDestroy() {
    removeListener(m_inputMethodCommit);
    removeListener(m_inputMethodGrabKeyboard);
    removeListener(m_inputMethodDestroy);
    removeListener(m_inputMethodNewPopup);
    removeListener(m_grabDestroy);
    m_inputMethod = nullptr;
    updateTextInputFocus();
    updateActiveTextInput();
  }
  void InputMethodRelay::handleInputMethodNewPopup(wlr_input_popup_surface_v2* popup) {
    auto entry = std::make_unique<Popup>();
    entry->relay = this;
    entry->popup = popup;
    entry->tree = wlr_scene_tree_create(m_server->imPopupTree());
    wlr_scene_subsurface_tree_create(entry->tree, popup->surface);
    entry->destroy.notify = onPopupDestroy;
    wl_signal_add(&popup->events.destroy, &entry->destroy);
    entry->surfaceCommit.notify = onPopupSurfaceCommit;
    wl_signal_add(&popup->surface->events.commit, &entry->surfaceCommit);
    m_popups.push_back(std::move(entry));
    updatePopupPosition(*m_popups.back());
  }

  void InputMethodRelay::handlePopupDestroy(Popup* popup) {
    std::erase_if(m_popups, [popup](const std::unique_ptr<Popup>& entry) { return entry.get() == popup; });
  }

  void InputMethodRelay::handleKeyboardFocusChange(void* data) {
    auto* event = static_cast<wlr_seat_keyboard_focus_change_event*>(data);
    m_focusedSurface = event->new_surface;
    updateTextInputFocus();
    updateActiveTextInput();
  }

  void InputMethodRelay::updateTextInputFocus() {
    for (const std::unique_ptr<TextInput>& textInput : m_textInputs) {
      wlr_surface* target = nullptr;
      if (m_inputMethod != nullptr
          && m_focusedSurface != nullptr
          && wl_resource_get_client(textInput->input->resource) == wl_resource_get_client(m_focusedSurface->resource)) {
        target = m_focusedSurface;
      }
      if (textInput->input->focused_surface == target) {
        continue;
      }
      if (textInput->input->focused_surface != nullptr) {
        wlr_text_input_v3_send_leave(textInput->input);
      }
      if (target != nullptr) {
        wlr_text_input_v3_send_enter(textInput->input, target);
      }
    }
  }

  void InputMethodRelay::updateActiveTextInput() {
    wlr_text_input_v3* candidate = nullptr;
    if (m_inputMethod != nullptr) {
      for (const std::unique_ptr<TextInput>& textInput : m_textInputs) {
        if (textInput->input->focused_surface != nullptr && textInput->input->current_enabled) {
          candidate = textInput->input;
          break;
        }
      }
    }
    if (candidate == m_activeTextInput) {
      return;
    }

    if (m_inputMethod != nullptr) {
      if (candidate != nullptr) {
        wlr_input_method_v2_send_activate(m_inputMethod);
      } else {
        wlr_input_method_v2_send_deactivate(m_inputMethod);
      }
      wlr_input_method_v2_send_done(m_inputMethod);
    }
    m_activeTextInput = candidate;
    if (m_activeTextInput != nullptr) {
      sendStateToInputMethod();
      updatePopupPositions();
    }
  }

  void InputMethodRelay::sendStateToInputMethod() {
    if (m_inputMethod == nullptr || m_activeTextInput == nullptr) {
      return;
    }

    const wlr_text_input_v3_state& current = m_activeTextInput->current;
    if ((m_activeTextInput->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) != 0) {
      wlr_input_method_v2_send_surrounding_text(
          m_inputMethod, current.surrounding.text, current.surrounding.cursor, current.surrounding.anchor
      );
    }
    wlr_input_method_v2_send_text_change_cause(m_inputMethod, current.text_change_cause);
    if ((m_activeTextInput->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) != 0) {
      wlr_input_method_v2_send_content_type(m_inputMethod, current.content_type.hint, current.content_type.purpose);
    }
    wlr_input_method_v2_send_done(m_inputMethod);
  }

  void InputMethodRelay::updatePopupPosition(Popup& popup) {
    if (m_activeTextInput == nullptr || m_focusedSurface == nullptr || !popup.popup->surface->mapped) {
      return;
    }

    wlr_box cursorRect{};
    if ((m_activeTextInput->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) != 0) {
      cursorRect = m_activeTextInput->current.cursor_rectangle;
    }

    wlr_surface* root = wlr_surface_get_root_surface(m_focusedSurface);
    if (wlr_xdg_surface* xdg = root != nullptr ? wlr_xdg_surface_try_from_wlr_surface(root) : nullptr;
        xdg != nullptr && xdg->data != nullptr) {
      auto* tree = static_cast<wlr_scene_tree*>(xdg->data);
      int lx = 0;
      int ly = 0;
      wlr_scene_node_coords(&tree->node, &lx, &ly);
      cursorRect.x += lx - xdg->geometry.x;
      cursorRect.y += ly - xdg->geometry.y;
    } else if (
        wlr_layer_surface_v1* layerSurface =
            root != nullptr ? wlr_layer_surface_v1_try_from_wlr_surface(root) : nullptr;
        layerSurface != nullptr && layerSurface->data != nullptr
    ) {
      auto* layer = static_cast<LayerSurface*>(layerSurface->data);
      int lx = 0;
      int ly = 0;
      wlr_scene_node_coords(&layer->scene()->tree->node, &lx, &ly);
      cursorRect.x += lx;
      cursorRect.y += ly;
    } else {
      return;
    }

    wlr_output* output = wlr_output_layout_output_at(m_server->outputLayout(), cursorRect.x, cursorRect.y);
    if (output == nullptr) {
      output = m_server->preferredOutput();
    }
    if (output == nullptr) {
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), output, &outputBox);

    wlr_xdg_positioner_rules rules{};
    rules.anchor_rect = cursorRect;
    rules.anchor = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    rules.gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    rules.constraint_adjustment = static_cast<xdg_positioner_constraint_adjustment>(
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
    );
    rules.size.width = popup.popup->surface->current.width;
    rules.size.height = popup.popup->surface->current.height;
    wlr_box popupBox{};
    wlr_xdg_positioner_rules_get_geometry(&rules, &popupBox);
    wlr_xdg_positioner_rules_unconstrain_box(&rules, &outputBox, &popupBox);
    wlr_scene_node_set_position(&popup.tree->node, popupBox.x, popupBox.y);

    wlr_box popupCursorRect{
        .x = cursorRect.x - popupBox.x,
        .y = cursorRect.y - popupBox.y,
        .width = cursorRect.width,
        .height = cursorRect.height,
    };
    wlr_input_popup_surface_v2_send_text_input_rectangle(popup.popup, &popupCursorRect);
  }

  void InputMethodRelay::updatePopupPositions() {
    for (const std::unique_ptr<Popup>& popup : m_popups) {
      updatePopupPosition(*popup);
    }
  }

} // namespace umbriel
