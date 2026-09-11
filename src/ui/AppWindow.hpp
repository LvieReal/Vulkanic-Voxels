#pragma once

#include <QMainWindow>

namespace vv::ui {

class VulkanWidget;

class AppWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit AppWindow(QWidget *parent = nullptr);
  ~AppWindow() override;

private:
  VulkanWidget *m_vulkanWidget = nullptr;
};

} // namespace vv::ui
