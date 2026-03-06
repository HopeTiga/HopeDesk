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
//  1. 添加设备弹窗
// ==========================================
class AddDeviceDialog : public QDialog {
    Q_OBJECT
public:
    QLineEdit *idEdit;
    QLineEdit *nameEdit;

    AddDeviceDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle("添加设备");
        setFixedSize(400, 340); // 加高一点
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);

        setStyleSheet(R"(
            QDialog { background-color: #26282D; border: 1px solid #4C4D52; border-radius: 12px; }
            QLabel { color: #E4E4E4; font-size: 14px; font-weight: bold; }
            QLineEdit { background-color: #1E1F22; border: 1px solid #3F4148; border-radius: 8px; color: #FFFFFF; padding: 10px; }
            QLineEdit:focus { border: 1px solid #337AFF; }
            QPushButton#btnSave { background-color: #337AFF; color: white; border-radius: 8px; padding: 8px; font-weight: bold; }
            QPushButton#btnSave:hover { background-color: #4C8AFF; }
            QPushButton#btnCancel { background-color: transparent; color: #9CA3AF; border: 1px solid #4C4D52; border-radius: 8px; padding: 8px; }
            QPushButton#btnCancel:hover { color: white; border-color: #606266; }
        )");

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 30, 30, 30);
        mainLayout->setSpacing(15);

        QLabel *title = new QLabel("添加新设备", this);
        title->setStyleSheet("font-size: 18px; margin-bottom: 5px;");
        title->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(title);

        mainLayout->addWidget(new QLabel("设备账号", this));
        idEdit = new QLineEdit(this);
        idEdit->setPlaceholderText("请输入对方ID");
        mainLayout->addWidget(idEdit);

        mainLayout->addWidget(new QLabel("备注名称", this));
        nameEdit = new QLineEdit(this);
        nameEdit->setPlaceholderText("例如: 公司电脑");
        mainLayout->addWidget(nameEdit);

        mainLayout->addStretch(1); // 顶到底部

        QHBoxLayout *btnLayout = new QHBoxLayout();
        QPushButton *btnCancel = new QPushButton("取消", this);
        btnCancel->setObjectName("btnCancel");
        QPushButton *btnSave = new QPushButton("立即添加", this);
        btnSave->setObjectName("btnSave");

        btnLayout->addWidget(btnCancel);
        btnLayout->addSpacing(10);
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
//  2. 登录/编辑资料弹窗 (修复布局和头像)
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
        setFixedSize(420, isEditMode ? 650 : 600); // 增加高度防止挤压
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);

        setStyleSheet(R"(
            QDialog { background-color: #26282D; border: 1px solid #4C4D52; border-radius: 12px; }
            QLabel { color: #DCDCDC; font-size: 13px; margin-bottom: 2px; }
            QLineEdit { background-color: #1E1F22; border: 1px solid #3F4148; border-radius: 8px; color: #FFFFFF; padding: 10px; }
            QLineEdit:focus { border: 1px solid #337AFF; }
            QLineEdit:read-only { color: #666; background: #151515; border: none; }
            QPushButton#btnConfirm { background-color: #337AFF; color: white; border-radius: 20px; font-weight: bold; font-size: 15px; }
            QPushButton#btnConfirm:hover { background-color: #4C8AFF; }
            QPushButton#btnClose { background: transparent; color: #999; font-size: 18px; border: none; }
            QPushButton#btnClose:hover { color: white; }
            QPushButton#btnLogout { background: transparent; color: #EF4444; border: 1px solid #EF4444; border-radius: 20px; }
            QPushButton#btnLogout:hover { background: rgba(239, 68, 68, 0.1); }
        )");

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(30, 20, 30, 30);
        mainLayout->setSpacing(10);

        // 顶部栏
        QHBoxLayout *topLayout = new QHBoxLayout();
        QLabel *title = new QLabel(isEditMode ? "个人信息" : "欢迎登录", this);
        title->setStyleSheet("font-size: 20px; font-weight: bold; color: white;");
        topLayout->addWidget(title);
        topLayout->addStretch();
        QPushButton *btnClose = new QPushButton("×", this);
        btnClose->setObjectName("btnClose");
        connect(btnClose, &QPushButton::clicked, this, &QDialog::reject); // 允许关闭
        topLayout->addWidget(btnClose);
        mainLayout->addLayout(topLayout);

        // 头像
        QHBoxLayout *avatarLayout = new QHBoxLayout();
        avatarLayout->addStretch();
        avatarDisplayLabel = new QLabel(this);
        avatarDisplayLabel->setFixedSize(100, 100);
        avatarDisplayLabel->setAlignment(Qt::AlignCenter);
        avatarDisplayLabel->setCursor(Qt::PointingHandCursor);
        avatarDisplayLabel->installEventFilter(this);
        resetAvatarStyle(); // 默认样式
        avatarLayout->addWidget(avatarDisplayLabel);
        avatarLayout->addStretch();
        mainLayout->addLayout(avatarLayout);

        QLabel *tip = new QLabel("点击更换头像", this);
        tip->setAlignment(Qt::AlignCenter);
        tip->setStyleSheet("color: #666; font-size: 12px; margin-bottom: 10px;");
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

        // 关键：把按钮挤到底部
        mainLayout->addStretch(1);

        // 按钮
        QPushButton *btnConfirm = new QPushButton(isEditMode ? "保存修改" : "立即登录", this);
        btnConfirm->setObjectName("btnConfirm");
        btnConfirm->setFixedHeight(40);
        mainLayout->addWidget(btnConfirm);

        if(isEditMode) {
            mainLayout->addSpacing(5);
            btnLogout = new QPushButton("退出登录", this);
            btnLogout->setObjectName("btnLogout");
            btnLogout->setFixedHeight(40);
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
        avatarDisplayLabel->setStyleSheet("background-color: #2F3136; border: 2px dashed #555; border-radius: 50px; color: #999; font-size: 24px;");
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
