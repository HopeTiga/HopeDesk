#include "FramelessWindowAgent.h"

#include <QCursor>
#include <QMouseEvent>
#include <QWindow>

namespace hope {
namespace rtc {
namespace {

constexpr int resizeBorderThickness = 8;

Qt::CursorShape calculateCursorShape(QWindow* window, const QPoint& pos)
{
    if (window->visibility() != QWindow::Windowed) return Qt::ArrowCursor;

    const int x = pos.x();
    const int y = pos.y();
    const int w = window->width();
    const int h = window->height();

    if ((x < resizeBorderThickness && y < resizeBorderThickness) ||
        (x >= w - resizeBorderThickness && y >= h - resizeBorderThickness)) {
        return Qt::SizeFDiagCursor;
    }
    if ((x >= w - resizeBorderThickness && y < resizeBorderThickness) ||
        (x < resizeBorderThickness && y >= h - resizeBorderThickness)) {
        return Qt::SizeBDiagCursor;
    }
    if (x < resizeBorderThickness || x >= w - resizeBorderThickness) {
        return Qt::SizeHorCursor;
    }
    if (y < resizeBorderThickness || y >= h - resizeBorderThickness) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}

Qt::Edges calculateWindowEdges(QWindow* window, const QPoint& pos)
{
    if (window->visibility() != QWindow::Windowed) return Qt::Edges();

    Qt::Edges edges;
    if (pos.x() < resizeBorderThickness) edges |= Qt::LeftEdge;
    if (pos.x() >= window->width() - resizeBorderThickness) edges |= Qt::RightEdge;
    if (pos.y() < resizeBorderThickness) edges |= Qt::TopEdge;
    if (pos.y() >= window->height() - resizeBorderThickness) edges |= Qt::BottomEdge;
    return edges;
}

}

FramelessWindowAgent::FramelessWindowAgent(QWidget* window, QObject* parent)
    : QObject(parent)
    , windowWidget(window)
    , windowHandle(nullptr)
    , windowStatus(Idle)
    , cursorShapeChanged(false)
{
    attachToWindow();
}

void FramelessWindowAgent::attachToWindow()
{
    if (!windowWidget) return;

    // Qt6 里 hide() 之后平台窗口会被销毁,重新 show() 时 windowHandle() 是新对象,必须重挂过滤器
    QWindow* currentHandle = windowWidget->windowHandle();
    if (currentHandle == windowHandle) return;

    if (windowHandle) windowHandle->removeEventFilter(this);
    windowHandle = currentHandle;
    if (windowHandle) windowHandle->installEventFilter(this);
}

void FramelessWindowAgent::addDraggableArea(QWidget* widget)
{
    if (!widget) return;
    draggableAreas.append(widget);
}

void FramelessWindowAgent::addInteractiveWidget(QWidget* widget)
{
    if (!widget) return;
    interactiveWidgets.append(widget);
}

QRect FramelessWindowAgent::sceneGeometry(const QWidget* widget, const QWidget* host)
{
    return QRect(widget->mapTo(host, QPoint(0, 0)), widget->size());
}

bool FramelessWindowAgent::isSizeFixed() const
{
    if (windowWidget->windowFlags() & Qt::MSWindowsFixedSizeDialogHint) return true;
    return windowWidget->minimumSize() == windowWidget->maximumSize();
}

bool FramelessWindowAgent::isInInteractiveWidget(const QPoint& pos) const
{
    for (const QPointer<QWidget>& widget : interactiveWidgets) {
        if (!widget || !widget->isVisible() || !widget->isEnabled()) continue;
        if (sceneGeometry(widget, windowWidget).contains(pos)) return true;
    }
    return false;
}

bool FramelessWindowAgent::isInDraggableArea(const QPoint& pos) const
{
    for (const QPointer<QWidget>& area : draggableAreas) {
        if (!area || !area->isVisible() || !area->isEnabled()) continue;
        if (!sceneGeometry(area, windowWidget).contains(pos)) continue;
        if (!isInInteractiveWidget(pos)) return true;
    }
    return false;
}

void FramelessWindowAgent::updateCursorShape(const QPoint& pos)
{
    if (isSizeFixed()) return;

    const Qt::CursorShape shape = calculateCursorShape(windowHandle, pos);
    if (shape == Qt::ArrowCursor) {
        if (cursorShapeChanged) {
            windowWidget->unsetCursor();
            cursorShapeChanged = false;
        }
        return;
    }

    windowWidget->setCursor(QCursor(shape));
    cursorShapeChanged = true;
}

bool FramelessWindowAgent::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != windowHandle) return QObject::eventFilter(watched, event);

    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease &&
        type != QEvent::MouseButtonDblClick && type != QEvent::MouseMove) {
        return false;
    }

    QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
    const QPoint scenePos = mouseEvent->scenePosition().toPoint();
    const bool fixedSize = isSizeFixed();
    const bool inDraggableArea = isInDraggableArea(scenePos);
    bool handled = false;

    switch (type) {
    case QEvent::MouseButtonPress: {
        windowStatus = WaitingRelease;
        if (mouseEvent->button() == Qt::LeftButton) {
            if (!fixedSize) {
                const Qt::Edges edges = calculateWindowEdges(windowHandle, scenePos);
                if (edges != Qt::Edges()) {
                    windowHandle->startSystemResize(edges);
                    windowStatus = Resizing;
                    handled = true;
                    break;
                }
            }
            if (inDraggableArea) {
                // 此处不能立即 startSystemMove:只按下不移动时收不到 MouseButtonRelease,
                // 所以推迟到真正产生 MouseMove 再启动系统移动
                windowStatus = PreparingMove;
                handled = true;
            }
        }
        break;
    }

    case QEvent::MouseButtonRelease: {
        if (windowStatus == Idle) {
            handled = inDraggableArea;
        } else if (windowStatus != WaitingRelease) {
            handled = true;
        }
        windowStatus = Idle;
        break;
    }

    case QEvent::MouseMove: {
        if (windowStatus == Idle || windowStatus == WaitingRelease) {
            updateCursorShape(scenePos);
        } else if (windowStatus == PreparingMove) {
            windowHandle->startSystemMove();
            windowStatus = Moving;
            handled = true;
        } else if (mouseEvent->buttons() == Qt::NoButton) {
            // 系统移动/缩放循环结束时按键释放事件可能被独占,靠无按键的 MouseMove 复位状态
            windowStatus = Idle;
            updateCursorShape(scenePos);
        } else {
            handled = true;
        }
        break;
    }

    case QEvent::MouseButtonDblClick: {
        const Qt::WindowStates state = windowWidget->windowState();
        if (mouseEvent->button() == Qt::LeftButton && inDraggableArea && !fixedSize &&
            (windowWidget->windowFlags() & Qt::WindowMaximizeButtonHint) &&
            !(state & Qt::WindowFullScreen)) {
            if (state & Qt::WindowMaximized) {
                windowWidget->setWindowState(state & ~Qt::WindowMaximized);
            } else {
                windowWidget->setWindowState(state | Qt::WindowMaximized);
            }
            handled = true;
        }
        break;
    }

    default:
        break;
    }

    if (handled) {
        event->accept();
        return true;
    }
    return false;
}

}
}
