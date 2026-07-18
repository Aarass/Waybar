#include "gtkmm/box.h"
#include "gtkmm/button.h"
#include "gtkmm/label.h"

namespace waybar::modules {

class Window {
 public:
  Window(const std::string& n);
  Gtk::Button& button() { return m_button; };
  void setActive(bool, int);

 private:
  Gtk::Button m_button;
  Gtk::Box m_box;
  Gtk::Label m_label;
};
}  // namespace waybar::modules
