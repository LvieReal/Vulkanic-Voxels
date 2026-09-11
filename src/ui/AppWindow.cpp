#include "ui/AppWindow.hpp"

#include "ui/VulkanWidget.hpp"

namespace vv::ui {

AppWindow::AppWindow(QWidget* parent) : QMainWindow(parent) {
	setWindowTitle("Vulkanic Voxels");

	m_vulkanWidget = new VulkanWidget(this);
	setCentralWidget(m_vulkanWidget);
}

AppWindow::~AppWindow() = default;

} // namespace vv::ui
