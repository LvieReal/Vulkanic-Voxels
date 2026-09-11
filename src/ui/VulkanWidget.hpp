#pragma once

#include <QWidget>
#include <cstdint>
#include <memory>

#include "core/Camera.hpp"
#include "core/GameTimer.hpp"

namespace vv::vulkan {
class VulkanRenderer;
}

namespace vv::ui {

// Native-rendering widget: hosts the Vulkan swapchain and the game loop.
//
// Input model (until the in-engine UI pass): the mouse is locked to the
// window center with a hidden cursor while playing. Escape releases the
// cursor and pauses the camera; Escape or a click re-locks it.
class VulkanWidget final : public QWidget {
	Q_OBJECT

 public:
	explicit VulkanWidget(QWidget* parent = nullptr);
	~VulkanWidget() override;

 protected:
	QPaintEngine* paintEngine() const override;
	bool eventFilter(QObject* watched, QEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void keyReleaseEvent(QKeyEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void focusOutEvent(QFocusEvent* event) override;

 private:
	void ensureInitialized();
	void tick();

	void lockMouse();
	void unlockMouse();
	void applyPendingMouseGrab();
	void setGamePaused(bool paused);
	void resetKeyStates();
	QPoint globalCenterPos() const;

	bool m_initialized = false;
	std::unique_ptr<vv::vulkan::VulkanRenderer> m_renderer;
	uint32_t m_pendingWidth = 0;
	uint32_t m_pendingHeight = 0;

	bool m_mouseLocked = false;
	// Set when the mouse should be grabbed but the platform window was not
	// visible yet; applied on the first expose event. This avoids Qt's
	// "setMouseGrabEnabled: Not setting mouse grab for invisible window"
	// warning.
	bool m_pendingMouseGrab = false;
	bool m_ignoreNextMouseMove = false;

	vv::core::Camera m_camera;
	vv::core::GameTimer m_gameTimer;
	glm::uvec3 m_chunkSizeVoxels = glm::uvec3(64u, 64u, 64u);
	glm::vec3 m_voxelSize = glm::vec3(1.0f, 1.0f, 1.0f);

	bool m_keyW = false;
	bool m_keyA = false;
	bool m_keyS = false;
	bool m_keyD = false;
	bool m_keySpace = false;
	bool m_keyCtrl = false;
	bool m_keyShift = false;
};

}  // namespace vv::ui
