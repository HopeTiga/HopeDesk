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

        setStyleSheet(R"(
            QDialog {
                background-color: #FFFFFF;
                border: 1px solid #E1E8ED;
                border-radius: 12px;
            }
            QLabel {
                color: #2C3E50;
                font-size: 14px;
                font-weight: bold;
            }
            QLineEdit {
                background-color: #F5F7FA;
                border: 1px solid #D6E3F0;
                border-radius: 8px;
                color: #333333;
                padding: 10px;
                font-size: 14px;
            }
            QLineEdit:focus {
                border: 1px solid #0072FF;
                background-color: #FFFFFF;
            }
            QLineEdit:hover {
                background-color: #FFFFFF;
            }
            QPushButton#btnSave {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0072FF, stop:1 #00B4FF);
                color: white;
                border-radius: 8px;
                padding: 10px;
                font-weight: bold;
                font-size: 14px;
                border: none;
            }
            QPushButton#btnSave:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #338CFF, stop:1 #33C3FF);
            }
            QPushButton#btnSave:pressed {
                background: #0056CC;
            }
            QPushButton#btnCancel {
                background-color: transparent;
                color: #5A6C7D;
                border: 1px solid #D6E3F0;
                border-radius: 8px;
                padding: 10px;
                font-size: 14px;
            }
            QPushButton#btnCancel:hover {
                color: #0072FF;
                border-color: #0072FF;
                background-color: rgba(0, 114, 255, 0.05);
            }
        )");

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 30, 30, 30);
        mainLayout->setSpacing(15);

        QLabel *title = new QLabel("添加新设备", this);
        title->setStyleSheet("font-size: 18px; color: #0072FF; margin-bottom: 5px;");
        title->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(title);

        // 分隔线
        QFrame *line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setStyleSheet("background-color: #E1E8ED; max-height: 1px; margin: 5px 0;");
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

        setStyleSheet(R"(
            QDialog {
                background-color: #FFFFFF;
                border: 1px solid #E1E8ED;
                border-radius: 12px;
            }
            QLabel {
                color: #5A6C7D;
                font-size: 13px;
                margin-bottom: 2px;
            }
            QLineEdit {
                background-color: #F5F7FA;
                border: 1px solid #D6E3F0;
                border-radius: 8px;
                color: #333333;
                padding: 10px;
                font-size: 14px;
            }
            QLineEdit:focus {
                border: 1px solid #0072FF;
                background-color: #FFFFFF;
            }
            QLineEdit:hover {
                background-color: #FFFFFF;
            }
            QLineEdit:read-only {
                color: #8C9AA8;
                background: #F0F4F8;
                border: 1px solid #E1E8ED;
            }
            QPushButton#btnConfirm {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0072FF, stop:1 #00B4FF);
                color: white;
                border-radius: 20px;
                font-weight: bold;
                font-size: 15px;
                border: none;
            }
            QPushButton#btnConfirm:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #338CFF, stop:1 #33C3FF);
            }
            QPushButton#btnConfirm:pressed {
                background: #0056CC;
            }
            QPushButton#btnClose {
                background: transparent;
                color: #8C9AA8;
                font-size: 20px;
                border: none;
                padding: 0;
                min-width: 30px;
                min-height: 30px;
            }
            QPushButton#btnClose:hover {
                color: #FF4D4F;
                background-color: rgba(255, 77, 79, 0.1);
                border-radius: 15px;
            }
            QPushButton#btnLogout {
                background: transparent;
                color: #FF4D4F;
                border: 1px solid #FF4D4F;
                border-radius: 20px;
            }
            QPushButton#btnLogout:hover {
                background: rgba(255, 77, 79, 0.1);
            }
        )");

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 20, 30, 30);
        mainLayout->setSpacing(12);

        // 顶部栏
        QHBoxLayout *topLayout = new QHBoxLayout();
        QLabel *title = new QLabel(isEditMode ? "个人信息" : "欢迎登录", this);
        title->setStyleSheet("font-size: 20px; font-weight: bold; color: #2C3E50;");
        topLayout->addWidget(title);
        topLayout->addStretch();
        QPushButton *btnClose = new QPushButton("×", this);
        btnClose->setObjectName("btnClose");
        btnClose->setCursor(Qt::PointingHandCursor);
        connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);
        topLayout->addWidget(btnClose);
        mainLayout->addLayout(topLayout);

        // 分隔线
        QFrame *line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setStyleSheet("background-color: #E1E8ED; max-height: 1px; margin: 5px 0 15px 0;");
        mainLayout->addWidget(line);

        // 头像
        QHBoxLayout *avatarLayout = new QHBoxLayout();
        avatarLayout->addStretch();
        avatarDisplayLabel = new QLabel(this);
        avatarDisplayLabel->setFixedSize(100, 100);
        avatarDisplayLabel->setAlignment(Qt::AlignCenter);
        avatarDisplayLabel->setCursor(Qt::PointingHandCursor);
        avatarDisplayLabel->installEventFilter(this);
        resetAvatarStyle();
        avatarLayout->addWidget(avatarDisplayLabel);
        avatarLayout->addStretch();
        mainLayout->addLayout(avatarLayout);

        QLabel *tip = new QLabel("点击更换头像", this);
        tip->setAlignment(Qt::AlignCenter);
        tip->setStyleSheet("color: #0072FF; font-size: 12px; margin-bottom: 10px;");
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
        avatarDisplayLabel->setText("📷");
        avatarDisplayLabel->setStyleSheet(R"(
            background-color: #F5F7FA;
            border: 2px dashed #D6E3F0;
            border-radius: 50px;
            color: #0072FF;
            font-size: 24px;
        )");
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
                    avatarDisplayLabel->setStyleSheet("background: transparent; border: none;");
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
                avatarDisplayLabel->setStyleSheet("background: transparent; border: none;");
                avatarDisplayLabel->setText("");
            }
        }
    }
};
