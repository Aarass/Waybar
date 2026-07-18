#include "modules/tmux/tmux.hpp"

#include <json/value.h>
#include <spdlog/spdlog.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "AModule.hpp"
#include "gtkmm/button.h"

// tmux set-hook [-g] hook-name[index] "command"
const auto HOOKS_INDEX = 99;
const auto HOOKS = {"after-select-window", "client-session-changed", "window-linked",
                    "window-unlinked"};

bool is_ancestor(pid_t start_pid, pid_t target_pid);

waybar::modules::Tmux::Tmux(const std::string& id, const Json::Value& config)
    : AModule(config, "tmux", id),
      tmux_event_loop_thread(&Tmux::tmux_event_loop, this),
      m_current_window_pid(std::nullopt),
      m_ipc(hyprland::IPC::inst()) {
  m_ipc.registerForIPC("activewindowv2", this);

  // auto onClick = [this]() {
  //   spdlog::info("Click");
  //
  //   for (auto& button : m_buttons) {
  //     button->set_label("O");
  //   }
  //
  //   auto& new_button = m_buttons.emplace_back(std::make_unique<Gtk::Button>("X"));
  //   new_button->show();
  //   m_box.pack_start(*new_button, false, false);
  // };

  // m_buttons.emplace_back(std::make_unique<Gtk::Button>("X"));
  // Gtk::Button& initial_button = *m_buttons.back();

  // initial_button->add_events(Gdk::BUTTON_PRESS_MASK);
  // initial_button->signal_clicked().connect(onClick);

  // auto& window = m_buttons.emplace_back(std::make_unique<Window>("X"));
  // m_box.pack_start(window->button(), false, false);
  event_box_.set_name("tmux");
  event_box_.add(m_box);

  // std::thread t(tmux_event_loop);
  // t.detach();  // or manage join() properly

  dp.emit();
}

waybar::modules::Tmux::~Tmux() {
  for (const auto* hook : HOOKS) {
    auto command = std::format("tmux set-hook -ug '{}[{}]'", hook, HOOKS_INDEX);
    std::system(command.c_str());
  }

  tmux_notification_loop_running_.store(false);

  if (tmux_notification_fd_.has_value()) {
    auto fd = tmux_notification_fd_.value();

    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
}

void waybar::modules::Tmux::Tmux::onEvent(const std::string& ev) {
  const auto separator = ev.find(">>");
  if (separator == std::string::npos) {
    spdlog::warn("Malformed Hyprland workspace event: {}", ev);
    return;
  }

  std::string eventName = ev.substr(0, separator);
  std::string payload = ev.substr(separator + 2);

  if (eventName == "activewindowv2") {
    auto windowAddress = payload;

    const auto clients = m_ipc.getSocket1JsonReply("clients");
    if (!clients.isArray()) return;

    auto it = std::ranges::find_if(clients, [&](const Json::Value& window) {
      if (window["address"].type() != Json::ValueType::stringValue)
        throw "Unexpected type received from hyprctl clients";

      auto clientWindowAddress = window["address"].asString();
      if (clientWindowAddress.starts_with("0x")) {
        clientWindowAddress = clientWindowAddress.substr(2);
      }

      return clientWindowAddress == windowAddress;
    });

    if (it == std::end(clients)) return;

    const auto& activeWindow = *it;
    if (!activeWindow.isObject()) throw "Unexpected type received from hyprctl clients";

    const auto& _activeWindowPid = activeWindow["pid"];
    if (!_activeWindowPid.isInt()) throw "Unexpected type received from hyprctl clients";

    m_current_window_pid = _activeWindowPid.asInt();
    dp.emit();
  }
}

auto waybar::modules::Tmux::update() -> void {
  if (!m_current_window_pid.has_value()) {
    clear();
    return;
  }

  auto current_window_pid = m_current_window_pid.value();

  bool found = false;
  int client_pid;
  int session_name;

  {
    FILE* f = popen("tmux list-clients -F '#{client_pid} #{session_name}'", "r");
    if (f == nullptr) return;

    while (fscanf(f, "%d %d", &client_pid, &session_name) == 2) {
      if (is_ancestor(client_pid, current_window_pid)) {
        found = true;
        break;
      }
    }

    pclose(f);
  }

  if (!found) {
    clear();
    return;
  }

  std::vector<int> window_statuses;
  {
    auto command = std::format("tmux list-windows -t {}  -F '#{{window_active}}'", session_name);
    spdlog::info("{}", command);
    FILE* f = popen(command.c_str(), "r");

    if (f == nullptr) return;

    int is_active;
    while (fscanf(f, "%d", &is_active) == 1) {
      window_statuses.push_back(is_active);
    }

    pclose(f);
  }

  if (window_statuses.size() == 0) {
    clear();
    return;
  }

  for (int status : window_statuses) {
    spdlog::info("W: {}", status);
  }

  this->render(window_statuses);

  // Call parent update
  AModule::update();
}

void waybar::modules::Tmux::clear() {
  for (auto& button : m_buttons) {
    button->button().hide();
  }
}

void waybar::modules::Tmux::render(std::span<const int> windows) {
  if (m_buttons.size() < windows.size()) {
    int diff = (windows.size() - m_buttons.size());
    for (int i = 0; i < diff; i++) {
      m_buttons.push_back(std::make_unique<Window>("O"));
      auto& button = m_buttons.back()->button();
      m_box.pack_start(button, false, false);
    }
  } else if (m_buttons.size() > windows.size()) {
    for (int i = windows.size(); i < m_buttons.size(); i++) {
      m_buttons[i]->button().hide();
    }
  }

  for (int i = 0; i < windows.size(); i++) {
    m_buttons[i]->setActive(windows[i] == 1, i);
    m_buttons[i]->button().show();
  }
}

namespace {
std::optional<std::string> CreateTempFile() {
  std::array<char, 64> dir = {"/tmp/waybar_tmuxXXXXXX"};

  if (mkdtemp(dir.data()) == nullptr) {
    spdlog::error("mkdtemp failed");
    return std::nullopt;
  }

  std::string file_path = std::string(dir.data()) + "/file";

  FILE* file_ptr = fopen(file_path.c_str(), "w");
  if (file_ptr == nullptr) {
    spdlog::error("Error creating temp file");
    return std::nullopt;
  }
  fclose(file_ptr);

  return file_path;
}
}  // namespace

void waybar::modules::Tmux::tmux_event_loop() {
  auto file_path_opt = CreateTempFile();
  if (!file_path_opt.has_value()) return;

  auto file_path = file_path_opt.value();
  spdlog::info("Created temp file for tmux notifications at '{}'", file_path);

  int fd = inotify_init();
  if (inotify_add_watch(fd, file_path.c_str(), IN_ATTRIB) == -1) {
    spdlog::error("Couldn't add watch");
    return;
  }
  tmux_notification_fd_ = fd;

  for (const auto* hook : HOOKS) {
    auto command = std::format("tmux set-hook -g '{}[{}]' 'run-shell \"touch {}\"'", hook,
                               HOOKS_INDEX, file_path);
    std::system(command.c_str());
  }

  std::array<char, sizeof(inotify_event)> buffer;
  while (tmux_notification_loop_running_.load()) {
    read(fd, buffer.data(), buffer.size());

    //-------//
    dp.emit();
    //------//
  }
  close(fd);
}

// void waybar::modules::Tmux::tmux_event_loop() {
//   FILE* f = popen("tmux -C", "r");
//   if (f == nullptr) return;
//
//   tmux_c = f;
//
//   std::array<char, 4096> buffer;
//
//   while (event_loop_running.load()) {
//     if (fgets(buffer.data(), buffer.size(), f) != nullptr) {
//       // TODO
//       // trigger update only on relevant events
//       dp.emit();
//     } else {
//       // fgets failed: could be EOF or error
//       // sleep briefly to avoid tight loop if tmux hiccups
//       std::this_thread::sleep_for(std::chrono::milliseconds(50));
//     }
//   }
//
//   pclose(f);
// }

static std::optional<pid_t> get_parent_pid(pid_t pid) {
  std::string path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream file(path);
  if (!file.is_open()) return std::nullopt;

  std::string content;
  std::getline(file, content);
  if (content.empty()) return std::nullopt;

  // format: pid (comm) state ppid ...
  std::istringstream iss(content);

  std::string tmp;

  // pid
  iss >> tmp;
  // comm (može da sadrži razmake, pa je u zagradama) – preskačemo ručno
  size_t pos1 = content.find('(');
  size_t pos2 = content.find(')');
  if (pos1 == std::string::npos || pos2 == std::string::npos) return -1;

  std::string after = content.substr(pos2 + 2);  // preskoči ") "
  std::istringstream iss2(after);

  std::string state;
  iss2 >> state;  // state

  pid_t ppid;
  iss2 >> ppid;  // parent pid
  return ppid;
}

bool is_ancestor(pid_t start_pid, pid_t target_pid) {
  pid_t current = start_pid;

  while (current > 0) {
    if (current == target_pid) return true;

    std::optional<pid_t> parent = get_parent_pid(current);
    if (!parent.has_value() || parent.value() == current) break;

    current = parent.value();
  }

  return false;
}

// Blocking read
// std::array<char, 1024> buffer;
// while (true) {
//   ssize_t n = read(client_fd, buffer.data(), sizeof(buffer) - 1);
//   if (n <= 0) {
//     break;
//   }
//   buffer[n] = '\0';
//
//   spdlog::info("Received data: '{}'", buffer.data());
// }
//
//
//
// Approach 2
// std::array<char, 64> dir = {"/tmp/waybar_tmuxXXXXXX"};
//
// if (mkdtemp(dir.data()) == nullptr) {
//   spdlog::error("Couldn't create a socket");
//   return;
// }
//
// std::string socket_path = std::string(dir.data()) + "/sock";
// spdlog::info("Path for socket: '{}'", socket_path);
//
// int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
// if (server_fd < 0) {
//   spdlog::error("Couldn't create a socket");
//   return;
// }
//
// // Remove old socket if exists
// unlink(socket_path.c_str());
//
// sockaddr_un addr;
// std::memset(&addr, 0, sizeof(addr));
// addr.sun_family = AF_UNIX;
// std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
//
// // Bind
// if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
//   spdlog::error("Couldn't bind socket");
//   return;
// }
//
// // Listen
// if (listen(server_fd, 5) < 0) {
//   spdlog::error("Couldn't prepare socket for listening");
//   return;
// }
//
// tmux_hooks_socket = server_fd;
//
// while (event_loop_running.load()) {
//   // Accept (blocking)
//   int client_fd = accept(server_fd, nullptr, nullptr);
//   if (client_fd < 0) {
//     spdlog::error("Error accepting socket connection");
//     return;
//   }
//
//   // Read and discard
//   std::array<char, 1024> buffer;
//   while (true) {
//     ssize_t n = read(client_fd, buffer.data(), sizeof(buffer) - 1);
//     if (n <= 0) break;
//   }
//
//   shutdown(client_fd, SHUT_RDWR);
//   close(client_fd);
//   dp.emit();
// }
//
// close(server_fd);
// unlink(socket_path.c_str());
