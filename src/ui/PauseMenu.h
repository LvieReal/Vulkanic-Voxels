#pragma once

#include <QWidget>

namespace vv::ui {

class PauseMenu final : public QWidget
{
    Q_OBJECT

public:
    explicit PauseMenu(QWidget* parent = nullptr);

signals:
    void backToGameRequested();
    void exitRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void applySizing();

    QWidget* m_panel = nullptr;
};

} // namespace vv::ui

