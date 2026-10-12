#pragma once

#include <QMouseEvent>
#include <QPaintEvent>
#include <QWidget>

namespace hope {
namespace rtc {

class SwitchButton : public QWidget
{
    Q_OBJECT

public:
    explicit SwitchButton(QWidget* parent = nullptr);

    bool isChecked() const;
    void setChecked(bool value);

Q_SIGNALS:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool checked = false;
};

}
}
