#pragma once

#include <memory>
#include <vector>
#include <wayland-server-core.h>

struct wlr_input_method_keyboard_grab_v2;
struct wlr_input_method_manager_v2;
struct wlr_input_method_v2;
struct wlr_input_popup_surface_v2;
struct wlr_keyboard;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_text_input_manager_v3;
struct wlr_text_input_v3;

namespace umbriel {

  class Server;

  // Bridges text input clients and the single input method client on the seat.
  // It forwards focus, state, and edits, and owns candidate popup scene nodes.
  class InputMethodRelay {
  public:
    explicit InputMethodRelay(Server& server);
    ~InputMethodRelay();

    InputMethodRelay(const InputMethodRelay&) = delete;
    InputMethodRelay& operator=(const InputMethodRelay&) = delete;

    // Returns the grab that should receive this keyboard, or null when there is
    // no grab or this is the input method's own virtual keyboard.
    [[nodiscard]] wlr_input_method_keyboard_grab_v2* grabForKeyboard(wlr_keyboard* keyboard) const;
    [[nodiscard]] bool ownsKeyboard(wlr_keyboard* keyboard) const;
    void canonicalizeOwnedKeyboardKeymap(wlr_keyboard* keyboard) const;

  private:
    struct TextInput {
      ~TextInput();

      InputMethodRelay* relay = nullptr;
      wlr_text_input_v3* input = nullptr;
      wl_listener enable{};
      wl_listener commit{};
      wl_listener disable{};
      wl_listener destroy{};
    };

    struct Popup {
      ~Popup();

      InputMethodRelay* relay = nullptr;
      wlr_input_popup_surface_v2* popup = nullptr;
      wlr_scene_tree* tree = nullptr;
      wl_listener destroy{};
      wl_listener surfaceCommit{};
    };

    static void onNewTextInput(wl_listener* listener, void* data);
    static void onTextInputEnable(wl_listener* listener, void* data);
    static void onTextInputCommit(wl_listener* listener, void* data);
    static void onTextInputDisable(wl_listener* listener, void* data);
    static void onTextInputDestroy(wl_listener* listener, void* data);
    static void onNewInputMethod(wl_listener* listener, void* data);
    static void onInputMethodCommit(wl_listener* listener, void* data);
    static void onInputMethodGrabKeyboard(wl_listener* listener, void* data);
    static void onInputMethodDestroy(wl_listener* listener, void* data);
    static void onInputMethodNewPopup(wl_listener* listener, void* data);
    static void onGrabDestroy(wl_listener* listener, void* data);
    static void onPopupDestroy(wl_listener* listener, void* data);
    static void onPopupSurfaceCommit(wl_listener* listener, void* data);
    static void onKeyboardFocusChange(wl_listener* listener, void* data);

    void handleNewTextInput(wlr_text_input_v3* input);
    void handleTextInputDestroy(TextInput* input);
    void handleNewInputMethod(wlr_input_method_v2* inputMethod);
    void handleInputMethodCommit();
    void handleInputMethodGrabKeyboard(wlr_input_method_keyboard_grab_v2* grab);
    void handleInputMethodDestroy();
    void handleInputMethodNewPopup(wlr_input_popup_surface_v2* popup);
    void handleGrabDestroy();
    void handlePopupDestroy(Popup* popup);
    void handleKeyboardFocusChange(void* data);
    void updateTextInputFocus();
    void updateActiveTextInput();
    void sendStateToInputMethod();
    void updatePopupPosition(Popup& popup);
    void updatePopupPositions();

    Server* m_server = nullptr;
    wlr_text_input_manager_v3* m_textInputManager = nullptr;
    wlr_input_method_manager_v2* m_inputMethodManager = nullptr;
    wlr_input_method_v2* m_inputMethod = nullptr;
    wlr_surface* m_focusedSurface = nullptr;
    wlr_text_input_v3* m_activeTextInput = nullptr;
    std::vector<std::unique_ptr<TextInput>> m_textInputs;
    std::vector<std::unique_ptr<Popup>> m_popups;
    wl_listener m_newTextInput{};
    wl_listener m_newInputMethod{};
    wl_listener m_keyboardFocusChange{};
    wl_listener m_inputMethodCommit{};
    wl_listener m_inputMethodGrabKeyboard{};
    wl_listener m_inputMethodDestroy{};
    wl_listener m_inputMethodNewPopup{};
    wl_listener m_grabDestroy{};
  };

} // namespace umbriel
