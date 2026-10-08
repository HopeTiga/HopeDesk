#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#include <QBuffer>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include "Theme.h"

// 辅助函数：创建高质量圆形头像
inline QPixmap createCircularAvatar(const QPixmap& source, int size) {
    if (source.isNull()) return QPixmap();
    QPixmap scaled = source.scaled(size * 2, size * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap result(size * 2, size * 2);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addEllipse(0, 0, size * 2, size * 2);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, scaled);
    painter.end();
    return result.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// ==========================================
//  通用确认/提示弹窗 (ToDesk 蓝白风格,替代 QMessageBox)
// ==========================================
class ConfirmDialog : public QDialog {
    Q_OBJECT
public:
    // cancelText 为空时只显示「确认」按钮(用作纯提示)
    ConfirmDialog(const QString& title, const QString& message,
                  const QString& confirmText = QStringLiteral("确认"),
                  const QString& cancelText = QStringLiteral("取消"),
                  QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(title);
        setFixedSize(420, 240);
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setObjectName("confirmDialog");

        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 28, 30, 28);
        mainLayout->setSpacing(16);

        QLabel* titleLabel = new QLabel(title, this);
        titleLabel->setObjectName("title");
        mainLayout->addWidget(titleLabel);

        QFrame* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setObjectName("dialogDivider");
        mainLayout->addWidget(line);

        QLabel* messageLabel = new QLabel(message, this);
        messageLabel->setObjectName("message");
        messageLabel->setWordWrap(true);
        mainLayout->addWidget(messageLabel);
        mainLayout->addStretch(1);

        QHBoxLayout* buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(12);
        buttonLayout->addStretch();

        if (!cancelText.isEmpty()) {
            QPushButton* cancelButton = new QPushButton(cancelText, this);
            cancelButton->setObjectName("btnCancel");
            cancelButton->setFixedHeight(40);
            cancelButton->setCursor(Qt::PointingHandCursor);
            connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
            buttonLayout->addWidget(cancelButton);
        }

        QPushButton* confirmButton = new QPushButton(confirmText, this);
        confirmButton->setObjectName("btnConfirm");
        confirmButton->setFixedHeight(40);
        confirmButton->setCursor(Qt::PointingHandCursor);
        connect(confirmButton, &QPushButton::clicked, this, &QDialog::accept);
        buttonLayout->addWidget(confirmButton);

        mainLayout->addLayout(buttonLayout);
    }
};

// ==========================================
//  1. 添加设备弹窗 (ToDesk 蓝白风格)
// ==========================================
class AddDeviceDialog : public QDialog {
    Q_OBJECT
public:
    QLineEdit *idEdit;
    QLineEdit *nameEdit;

    AddDeviceDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle("添加设备");
        setFixedSize(400, 340);
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setObjectName("addDeviceDialog");

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 30, 30, 30);
        mainLayout->setSpacing(15);

        QLabel *title = new QLabel("添加新设备", this);
        title->setObjectName("dlgTitle");
        title->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(title);

        QFrame *line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setObjectName("dialogDivider");
        mainLayout->addWidget(line);

        mainLayout->addSpacing(10);

        mainLayout->addWidget(new QLabel("设备账号", this));
        idEdit = new QLineEdit(this);
        idEdit->setPlaceholderText("请输入对方ID");
        mainLayout->addWidget(idEdit);

        mainLayout->addWidget(new QLabel("备注名称", this));
        nameEdit = new QLineEdit(this);
        nameEdit->setPlaceholderText("例如: 公司电脑");
        mainLayout->addWidget(nameEdit);

        mainLayout->addStretch(1);

        QHBoxLayout *btnLayout = new QHBoxLayout();
        btnLayout->setSpacing(12);

        QPushButton *btnCancel = new QPushButton("取消", this);
        btnCancel->setObjectName("btnCancel");
        btnCancel->setFixedHeight(40);

        QPushButton *btnSave = new QPushButton("立即添加", this);
        btnSave->setObjectName("btnSave");
        btnSave->setFixedHeight(40);

        btnLayout->addWidget(btnCancel);
        btnLayout->addWidget(btnSave);
        mainLayout->addLayout(btnLayout);

        connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(btnSave, &QPushButton::clicked, [this]() {
            if(idEdit->text().trimmed().isEmpty()) return;
            accept();
        });
    }

    QString getID() const { return idEdit->text().trimmed(); }
    QString getName() const { return nameEdit->text().trimmed(); }
};

// ==========================================
//  2. 登录/编辑资料弹窗 (ToDesk 蓝白风格)
// ==========================================
class LoginDialog : public QDialog {
    Q_OBJECT
public:
    QLineEdit *accountEdit;
    QLineEdit *passwordEdit;
    QLineEdit *nickEdit;
    QString customAvatarPath;
    QLabel *avatarDisplayLabel;
    QPushButton *btnLogout = nullptr;

Q_SIGNALS:
    void logoutRequested();

public:
    LoginDialog(QWidget *parent = nullptr, bool isEditMode = false) : QDialog(parent) {
        setFixedSize(420, isEditMode ? 650 : 600);
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setObjectName("loginDialog");

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 20, 30, 30);
        mainLayout->setSpacing(12);

        QHBoxLayout *topLayout = new QHBoxLayout();
        QLabel *title = new QLabel(isEditMode ? "个人信息" : "欢迎登录", this);
        title->setObjectName("loginTitle");
        topLayout->addWidget(title);
        topLayout->addStretch();
        QPushButton *btnClose = new QPushButton(this);
        btnClose->setObjectName("btnClose");
        btnClose->setIconSize(QSize(14, 14));
        btnClose->setIcon(hope::rtc::theme::icon(QStringLiteral(":/icons/close.svg"), hope::rtc::theme::color("textFaint"), 14));
        btnClose->setCursor(Qt::PointingHandCursor);
        connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);
        topLayout->addWidget(btnClose);
        mainLayout->addLayout(topLayout);

        QFrame *line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setObjectName("dialogDivider");
        mainLayout->addWidget(line);

        QHBoxLayout *avatarLayout = new QHBoxLayout();
        avatarLayout->addStretch();
        avatarDisplayLabel = new QLabel(this);
        avatarDisplayLabel->setObjectName("avatarDrop");
        avatarDisplayLabel->setFixedSize(100, 100);
        avatarDisplayLabel->setAlignment(Qt::AlignCenter);
        avatarDisplayLabel->setCursor(Qt::PointingHandCursor);
        avatarDisplayLabel->installEventFilter(this);
        resetAvatarStyle();
        avatarLayout->addWidget(avatarDisplayLabel);
        avatarLayout->addStretch();
        mainLayout->addLayout(avatarLayout);

        QLabel *tip = new QLabel("点击更换头像", this);
        tip->setObjectName("avatarTip");
        tip->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(tip);

        // 表单
        auto addInput = [&](const QString& txt, QLineEdit*& edit, bool isPwd=false, bool ro=false) {
            mainLayout->addWidget(new QLabel(txt, this));
            edit = new QLineEdit(this);
            if(isPwd) edit->setEchoMode(QLineEdit::Password);
            if(ro) edit->setReadOnly(true);
            mainLayout->addWidget(edit);
        };

        addInput("账号 (ID)", accountEdit, false, isEditMode);
        addInput("密码 (本地验证)", passwordEdit, true);
        addInput("昵称", nickEdit);

        mainLayout->addStretch(1);

        // 按钮
        QPushButton *btnConfirm = new QPushButton(isEditMode ? "保存修改" : "立即登录", this);
        btnConfirm->setObjectName("btnConfirm");
        btnConfirm->setFixedHeight(44);
        btnConfirm->setCursor(Qt::PointingHandCursor);
        mainLayout->addWidget(btnConfirm);

        if(isEditMode) {
            mainLayout->addSpacing(10);
            btnLogout = new QPushButton("退出登录", this);
            btnLogout->setObjectName("btnLogout");
            btnLogout->setFixedHeight(44);
            btnLogout->setCursor(Qt::PointingHandCursor);
            mainLayout->addWidget(btnLogout);
            connect(btnLogout, &QPushButton::clicked, this, [this](){
                Q_EMIT logoutRequested();
                reject();
            });
        }

        connect(btnConfirm, &QPushButton::clicked, [this]() {
            if(accountEdit->text().trimmed().isEmpty()) return;
            accept();
        });
    }

    void resetAvatarStyle() {
        avatarDisplayLabel->setText("");
        avatarDisplayLabel->setPixmap(hope::rtc::theme::icon(QStringLiteral(":/icons/camera.svg"), hope::rtc::theme::color("primary"), 28).pixmap(28, 28));
        hope::rtc::theme::setState(avatarDisplayLabel, "state", QStringLiteral("empty"));
    }

    bool eventFilter(QObject *obj, QEvent *event) override {
        if(obj == avatarDisplayLabel && event->type() == QEvent::MouseButtonPress) {
            QString path = QFileDialog::getOpenFileName(this, "选择头像", "", "Images (*.png *.jpg)");
            if(!path.isEmpty()) {
                QPixmap p(path);
                if(!p.isNull()) {
                    if(p.width() > 200) p = p.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); p.save(&buffer, "PNG");
                    customAvatarPath = QString::fromLatin1(bytes.toBase64());

                    avatarDisplayLabel->setPixmap(createCircularAvatar(p, 50));
                    hope::rtc::theme::setState(avatarDisplayLabel, "state", QStringLiteral("image"));
                    avatarDisplayLabel->setText("");
                }
            }
            return true;
        }
        return QDialog::eventFilter(obj, event);
    }

    void setValues(const QString& acc, const QString& pwd, const QString& name, const QString& avatar) {
        accountEdit->setText(acc);
        passwordEdit->setText(pwd);
        nickEdit->setText(name);
        customAvatarPath = avatar;
        if(!avatar.isEmpty()) {
            QPixmap p; p.loadFromData(QByteArray::fromBase64(avatar.toLatin1()));
            if(!p.isNull()) {
                avatarDisplayLabel->setPixmap(createCircularAvatar(p, 50));
                hope::rtc::theme::setState(avatarDisplayLabel, "state", QStringLiteral("image"));
                avatarDisplayLabel->setText("");
            }
        }
    }
};
