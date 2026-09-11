#include "ui/PauseMenu.hpp"

#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace vv::ui {

PauseMenu::PauseMenu(QWidget* parent) : QWidget(parent) {
	setAutoFillBackground(false);
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet("background-color: rgba(0, 0, 0, 128);");

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setAlignment(Qt::AlignCenter);

	m_panel = new QWidget(this);
	m_panel->setAttribute(Qt::WA_StyledBackground, true);
	m_panel->setStyleSheet(
		"background-color: rgba(20, 20, 20, 220);"
		"border: 1px solid rgba(255, 255, 255, 35);"
		"border-radius: 12px;");

	auto* panelLayout = new QVBoxLayout(m_panel);
	panelLayout->setContentsMargins(24, 24, 24, 24);
	panelLayout->setSpacing(12);

	auto* back = new QPushButton("Back To Game", m_panel);
	auto* exit = new QPushButton("Exit", m_panel);

	back->setStyleSheet("font-size: 18px; padding: 12px;");
	exit->setStyleSheet("font-size: 18px; padding: 12px;");

	connect(back, &QPushButton::clicked, this, &PauseMenu::backToGameRequested);
	connect(exit, &QPushButton::clicked, this, &PauseMenu::exitRequested);

	panelLayout->addWidget(back);
	panelLayout->addWidget(exit);

	root->addWidget(m_panel);

	applySizing();
}

void PauseMenu::resizeEvent(QResizeEvent* event) {
	QWidget::resizeEvent(event);
	applySizing();
}

void PauseMenu::applySizing() {
	if (!m_panel) {
		return;
	}

	const int w = width();
	const int h = height();
	const int buttonW = std::max(200, static_cast<int>(w * 0.2));
	const int buttonH = std::max(44, static_cast<int>(h * 0.08));
	const int panelW = buttonW + 48;
	const int panelH =
		buttonH * 2 + 24 + 24 + 12; // 2 buttons + margins + spacing

	m_panel->setFixedSize(panelW, panelH);

	const auto buttons = m_panel->findChildren<QPushButton*>();
	for (auto* b : buttons) {
		b->setFixedSize(buttonW, buttonH);
	}
}

} // namespace vv::ui
