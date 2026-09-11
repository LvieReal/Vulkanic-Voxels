#include "ui/VulkanWidget.hpp"

#include <windows.h>

#include <QCursor>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>

#include "ui/PauseMenu.hpp"
#include "vulkan/VulkanRenderer.hpp"

namespace vv::ui {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
	setAttribute(Qt::WA_NativeWindow, true);
	setAttribute(Qt::WA_PaintOnScreen, true);
	setAttribute(Qt::WA_NoSystemBackground, true);

	setFocusPolicy(Qt::StrongFocus);

	// Ensure the native HWND exists early, so we can create the VkSurfaceKHR.
	(void)winId();

	// Reasonable defaults for a first-person style free camera.
	m_camera.setFovDegrees(70.0f);
	m_camera.setMoveSpeed(40.0f);

	auto* timer = new QTimer(this);
	timer->setTimerType(Qt::PreciseTimer);
	connect(timer, &QTimer::timeout, this, [this]() { tick(); });
	timer->start(16);

	m_pauseMenu = new PauseMenu(this);
	m_pauseMenu->hide();
	m_pauseMenu->setGeometry(rect());
	connect(m_pauseMenu, &PauseMenu::backToGameRequested, this,
					[this]() { hidePauseMenu(); });
	connect(m_pauseMenu, &PauseMenu::exitRequested, this, [this]() {
		if (auto* w = window()) {
			w->close();
		}
	});
}

VulkanWidget::~VulkanWidget() = default;

QPaintEngine* VulkanWidget::paintEngine() const {
	return nullptr;
}

void VulkanWidget::showEvent(QShowEvent* event) {
	QWidget::showEvent(event);

	const qreal dpr = devicePixelRatioF();
	m_pendingWidth =
		static_cast<uint32_t>(std::max(1, static_cast<int>(width() * dpr)));
	m_pendingHeight =
		static_cast<uint32_t>(std::max(1, static_cast<int>(height() * dpr)));
	ensureInitialized();
	lockMouse();
}

void VulkanWidget::resizeEvent(QResizeEvent* event) {
	QWidget::resizeEvent(event);

	const qreal dpr = devicePixelRatioF();
	m_pendingWidth = static_cast<uint32_t>(
		std::max(1, static_cast<int>(event->size().width() * dpr)));
	m_pendingHeight = static_cast<uint32_t>(
		std::max(1, static_cast<int>(event->size().height() * dpr)));
	if (m_renderer) {
		m_renderer->resize(m_pendingWidth, m_pendingHeight);
	}

	if (m_pauseMenu) {
		m_pauseMenu->setGeometry(rect());
	}
}

void VulkanWidget::ensureInitialized() {
	if (m_initialized) {
		return;
	}

	const auto hwnd = reinterpret_cast<HWND>(winId());
	if (!hwnd) {
		QMessageBox::critical(this, "Vulkan",
													"Failed to obtain a native window handle (HWND).");
		return;
	}

	m_renderer = std::make_unique<vv::vulkan::VulkanRenderer>();
	m_renderer->setWorldConfig(m_chunkSizeVoxels, m_voxelSize);

	vv::vulkan::VulkanRenderer::InitInfo init{};
	init.hinstance = GetModuleHandleW(nullptr);
	init.hwnd = hwnd;
	init.width = m_pendingWidth;
	init.height = m_pendingHeight;

	std::string error;
	if (!m_renderer->init(init, error)) {
		QMessageBox::critical(this, "Vulkan unsupported",
													QString::fromStdString(error));
		m_renderer.reset();
		return;
	}

	// Start at a sensible place relative to the initial chunk (0..64).
	const glm::vec3 chunkSizeWorld = glm::vec3(m_chunkSizeVoxels) * m_voxelSize;
	const glm::vec3 center = chunkSizeWorld * 0.5f;
	m_camera.setPosition(center + glm::vec3(0.0f, chunkSizeWorld.y * 0.35f,
																					chunkSizeWorld.z * 1.75f));
	m_camera.setYawPitchDegrees(180.0f, -10.0f);
	m_gameTimer.reset();

	m_initialized = true;
}

void VulkanWidget::tick() {
	if (!m_renderer) {
		return;
	}

	const float dt = std::min(0.050f, m_gameTimer.tickSeconds());

	glm::vec3 moveLocal(0.0f);
	if (m_keyA) {
		moveLocal.x -= 1.0f;
	}
	if (m_keyD) {
		moveLocal.x += 1.0f;
	}
	if (m_keyCtrl) {
		moveLocal.y -= 1.0f;
	}
	if (m_keySpace) {
		moveLocal.y += 1.0f;
	}
	if (m_keyW) {
		moveLocal.z += 1.0f;
	}
	if (m_keyS) {
		moveLocal.z -= 1.0f;
	}

	const float speedMul = m_keyShift ? 3.0f : 1.0f;
	m_camera.moveLocal(moveLocal, dt, speedMul);

	m_renderer->setCamera(m_camera,
												static_cast<float>(m_gameTimer.totalSeconds()));
	m_renderer->drawFrame();
}

void VulkanWidget::keyPressEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyPressEvent(event);
		return;
	}

	if (event->key() == Qt::Key_Escape) {
		if (m_pauseMenu && m_pauseMenu->isVisible()) {
			hidePauseMenu();
		} else {
			showPauseMenu();
		}
		event->accept();
		return;
	}

	switch (event->key()) {
		case Qt::Key_W:
			m_keyW = true;
			break;
		case Qt::Key_A:
			m_keyA = true;
			break;
		case Qt::Key_S:
			m_keyS = true;
			break;
		case Qt::Key_D:
			m_keyD = true;
			break;
		case Qt::Key_Space:
			m_keySpace = true;
			break;
		case Qt::Key_Control:
			m_keyCtrl = true;
			break;
		case Qt::Key_Shift:
			m_keyShift = true;
			break;
		default:
			break;
	}

	QWidget::keyPressEvent(event);
}

void VulkanWidget::keyReleaseEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyReleaseEvent(event);
		return;
	}

	switch (event->key()) {
		case Qt::Key_W:
			m_keyW = false;
			break;
		case Qt::Key_A:
			m_keyA = false;
			break;
		case Qt::Key_S:
			m_keyS = false;
			break;
		case Qt::Key_D:
			m_keyD = false;
			break;
		case Qt::Key_Space:
			m_keySpace = false;
			break;
		case Qt::Key_Control:
			m_keyCtrl = false;
			break;
		case Qt::Key_Shift:
			m_keyShift = false;
			break;
		default:
			break;
	}

	QWidget::keyReleaseEvent(event);
}

void VulkanWidget::mouseMoveEvent(QMouseEvent* event) {
	if (!m_mouseLocked) {
		QWidget::mouseMoveEvent(event);
		return;
	}

	if (m_ignoreNextMouseMove) {
		m_ignoreNextMouseMove = false;
		event->accept();
		return;
	}

	const QPoint center = globalCenterPos();
	const QPointF gpos = event->globalPosition();
	const float dx =
		static_cast<float>(gpos.x() - static_cast<qreal>(center.x()));
	const float dy =
		static_cast<float>(gpos.y() - static_cast<qreal>(center.y()));
	if (dx == 0.0f && dy == 0.0f) {
		event->accept();
		return;
	}

	m_camera.addMouseDeltaPixels(dx, dy);

	m_ignoreNextMouseMove = true;
	QCursor::setPos(center);
	event->accept();
}

void VulkanWidget::mousePressEvent(QMouseEvent* event) {
	(void)event;

	QWidget::mousePressEvent(event);
}

void VulkanWidget::focusOutEvent(QFocusEvent* event) {
	showPauseMenu();
	QWidget::focusOutEvent(event);
}

QPoint VulkanWidget::globalCenterPos() const {
	const QPoint local = rect().center();
	return mapToGlobal(local);
}

void VulkanWidget::lockMouse() {
	if (m_mouseLocked) {
		return;
	}

	m_mouseLocked = true;
	setCursor(Qt::BlankCursor);
	grabMouse();
	grabKeyboard();

	m_ignoreNextMouseMove = true;
	QCursor::setPos(globalCenterPos());
}

void VulkanWidget::unlockMouse() {
	if (!m_mouseLocked) {
		return;
	}

	m_mouseLocked = false;
	unsetCursor();
	releaseMouse();
	releaseKeyboard();
}

void VulkanWidget::showPauseMenu() {
	if (!m_pauseMenu) {
		return;
	}
	if (m_pauseMenu->isVisible()) {
		return;
	}

	m_gameTimer.setPaused(true);
	unlockMouse();

	m_pauseMenu->setGeometry(rect());
	m_pauseMenu->show();
	m_pauseMenu->raise();
	m_pauseMenu->setFocus(Qt::ActiveWindowFocusReason);
}

void VulkanWidget::hidePauseMenu() {
	if (!m_pauseMenu) {
		return;
	}
	if (!m_pauseMenu->isVisible()) {
		return;
	}

	m_pauseMenu->hide();
	m_gameTimer.setPaused(false);
	lockMouse();
}

} // namespace vv::ui
