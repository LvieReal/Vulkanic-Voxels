#include "ui/AppWindow.h"

#include "ui/VulkanWidget.h"

namespace vv::ui {

AppWindow::AppWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Window");

    m_vulkanWidget = new VulkanWidget(this);
    setCentralWidget(m_vulkanWidget);
}

AppWindow::~AppWindow() = default;

} // namespace vv::ui

