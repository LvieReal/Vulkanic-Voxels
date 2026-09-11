#pragma once

#include <QWidget>

#include "core/Camera.h"
#include "core/GameTimer.h"

#include <cstdint>
#include <memory>

namespace vv::vulkan {
class VulkanRenderer;
}

namespace vv::ui {

class PauseMenu;

class VulkanWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);
    ~VulkanWidget() override;

protected:
    QPaintEngine* paintEngine() const override;
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
    QPoint globalCenterPos() const;

    void showPauseMenu();
    void hidePauseMenu();

    bool m_initialized = false;
    std::unique_ptr<vv::vulkan::VulkanRenderer> m_renderer;
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;

    bool m_mouseLocked = false;
    bool m_ignoreNextMouseMove = false;

    vv::core::Camera m_camera;
    vv::core::GameTimer m_gameTimer;
    PauseMenu* m_pauseMenu = nullptr;
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

} // namespace vv::ui
