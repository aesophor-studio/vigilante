// Copyright (c) 2018-2024 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#ifndef VIGILANTE_UI_WINDOW_MANAGER_H_
#define VIGILANTE_UI_WINDOW_MANAGER_H_

#include <memory>
#include <vector>

#include <axmol.h>
#include "ui/Window.h"

namespace vigilante {

// A simple stacking/floating window manager.
class WindowManager final {
 public:
  WindowManager();

  void update(const float delta);

  // Push `window` to the internal window stack and render it.
  void push(std::unique_ptr<Window> window);

  // Pop the top window off the internal window stack and unrender it.
  std::unique_ptr<Window> pop();

  inline Window* top() const { return isEmpty() ? nullptr : _windows.back().get(); }
  inline bool isEmpty() const { return _windows.empty(); }
  inline int getSize() const { return static_cast<int>(_windows.size()); }
  inline void setScene(ax::Scene* scene) { _scene = scene; }

 private:
  const size_t _kMaxWindowCount;

  ax::Scene* _scene{};
  std::vector<std::unique_ptr<Window>> _windows;
};

}  // namespace vigilante

#endif  // VIGILANTE_UI_WINDOW_MANAGER_H_
