#include "SwitchButton.h"

#include "Theme.h"

#include <QPainter>

namespace hope {
namespace rtc {

SwitchButton::SwitchButton(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(44, 24);
    setCursor(Qt::PointingHandCursor);
}

bool SwitchButton::isChecked() const
{
    return checked;
}

void SwitchButton::setChecked(bool value)
{
    if (checked == value) return;
    checked = value;
    update();
    Q_EMIT toggled(checked);
}

void SwitchButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    const qreal radius = height() / 2.0;
    painter.setBrush(theme::color(checked ? "primaryInk" : "borderStrong"));
    painter.drawRoundedRect(QRectF(rect()), radius, radius);

    const qreal knobRadius = radius - 3.0;
    const qreal knobCenterX = checked ? width() - radius : radius;
    painter.setBrush(theme::color("bgCard"));
    painter.drawEllipse(QPointF(knobCenterX, radius), knobRadius, knobRadius);
}

void SwitchButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        setChecked(!checked);
    }
    QWidget::mouseReleaseEvent(event);
}

}
}
