#pragma once

#include <sched.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AModule.hpp"
#include "modules/hyprland/backend.hpp"
#include "window.hpp"

namespace waybar::modules {

class Tmux : public AModule, public hyprland::EventHandler {
 public:
  Tmux(const std::string&, const Json::Value&);
  ~Tmux() override;

  void onEvent(const std::string& e) override;
  auto update() -> void override;

  void clear();
  void render(std::span<const int> windows);

 private:
  void tmux_event_loop();
  FILE* tmux_c = nullptr;
  std::optional<int> tmux_hooks_socket;
  std::optional<int> tmux_notification_fd_;
  std::atomic<bool> tmux_notification_loop_running_{true};
  std::thread tmux_event_loop_thread;

  std::optional<int> m_current_window_pid;

  std::vector<std::unique_ptr<Window>> m_buttons;
  Gtk::Box m_box;
  hyprland::IPC& m_ipc;
};

}  // namespace waybar::modules
