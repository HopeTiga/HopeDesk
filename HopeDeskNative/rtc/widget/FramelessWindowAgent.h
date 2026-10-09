// 无边框窗口逻辑移植自 QWindowKit(Apache-2.0):
// Copyright (C) 2021-2023 wangwenx190
// Copyright (C) 2023-present Stdware Collections(https://github.com/stdware)
// 仅移植其纯 Qt 实现(qtwindowcontext.cpp / widgetitemdelegate.cpp),不含 Win32 分支。

#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QPoint;
class QWindow;
QT_END_NAMESPACE

namespace hope {
namespace rtc {

class FramelessWindowAgent : public QObject
{
    Q_OBJECT

public:
    explicit FramelessWindowAgent(QWidget* window, QObject* parent = nullptr);

    void attachToWindow();
    void addDraggableArea(QWidget* widget);
    void addInteractiveWidget(QWidget* widget);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum Status { Idle, WaitingRelease, PreparingMove, Moving, Resizing };

    bool isSizeFixed() const;
    bool isInInteractiveWidget(const QPoint& pos) const;
    bool isInDraggableArea(const QPoint& pos) const;
    void updateCursorShape(const QPoint& pos);

    static QRect sceneGeometry(const QWidget* widget, const QWidget* host);

    QWidget* windowWidget;
    QWindow* windowHandle;
    QVector<QPointer<QWidget> > draggableAreas;
    QVector<QPointer<QWidget> > interactiveWidgets;
    Status windowStatus;
    bool cursorShapeChanged;
};

}
}
