#include "include/modules/tmux/window.hpp"

#include <string>

#include "gtkmm/enums.h"
#include "gtkmm/label.h"

waybar::modules::Window::Window(const std::string& n) : m_button(), m_box(), m_label(n) {
  m_label.get_style_context()->add_class("label");

  m_box.get_style_context()->add_class("content");

  m_button.set_relief(Gtk::RELIEF_NONE);

  m_box.set_center_widget(m_label);
  m_button.add(m_box);

  m_button.show_all();
}

void waybar::modules::Window::setActive(bool isActive, int index) {
  // m_label.set_label(isActive ? "X" : "O");
  m_label.set_label("");
  // m_label.set_label(std::to_string(index + 1));

  auto styleContext = m_button.get_style_context();

  if (isActive) {
    styleContext->add_class("active");
  } else {
    if (styleContext->has_class("active")) {
      styleContext->remove_class("active");
    }
  }
}
