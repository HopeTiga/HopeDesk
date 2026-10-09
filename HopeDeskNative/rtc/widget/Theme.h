#pragma once

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QString>
#include <QSvgRenderer>
#include <QWidget>

namespace hope {
namespace rtc {
namespace theme {

inline const QHash<QString, QString>& tokens()
{
    static const QHash<QString, QString> table = {
        { QStringLiteral("primary"),           QStringLiteral("#8CBEF4") },
        { QStringLiteral("primaryInk"),        QStringLiteral("#6FA9EC") },
        { QStringLiteral("primaryLight"),      QStringLiteral("#C8E0FA") },
        { QStringLiteral("primaryHover"),      QStringLiteral("#7AB2F0") },
        { QStringLiteral("primaryPressed"),    QStringLiteral("#639FE6") },
        { QStringLiteral("primarySoft"),       QStringLiteral("#F3F9FE") },
        { QStringLiteral("primarySoftBorder"), QStringLiteral("#DEF0FD") },
        { QStringLiteral("primaryHoverSoft"),  QStringLiteral("rgba(140, 190, 244, 0.18)") },

        { QStringLiteral("bg"),                QStringLiteral("#F7F9FC") },
        { QStringLiteral("bgSideBar"),         QStringLiteral("#FFFFFF") },
        { QStringLiteral("bgCard"),            QStringLiteral("#FFFFFF") },
        { QStringLiteral("bgInput"),           QStringLiteral("#F5F8FC") },
        { QStringLiteral("bgInputFocus"),      QStringLiteral("#FFFFFF") },
        { QStringLiteral("navActive"),         QStringLiteral("#F3F9FE") },
        { QStringLiteral("navHover"),          QStringLiteral("rgba(140, 190, 244, 0.14)") },

        { QStringLiteral("border"),            QStringLiteral("#E2E7F0") },
        { QStringLiteral("borderStrong"),      QStringLiteral("#DFE5EE") },

        { QStringLiteral("text"),              QStringLiteral("#1F2A37") },
        { QStringLiteral("textSub"),           QStringLiteral("#6B7A8D") },
        { QStringLiteral("textFaint"),         QStringLiteral("#98A4B3") },

        { QStringLiteral("success"),           QStringLiteral("#22B573") },
        { QStringLiteral("successSoft"),       QStringLiteral("rgba(34, 181, 115, 0.12)") },
        { QStringLiteral("successSoftBorder"), QStringLiteral("rgba(34, 181, 115, 0.30)") },
        { QStringLiteral("danger"),            QStringLiteral("#F0483E") },
        { QStringLiteral("dangerSoft"),        QStringLiteral("rgba(240, 72, 62, 0.10)") },
        { QStringLiteral("warn"),              QStringLiteral("#E6A23C") },
        { QStringLiteral("warnSoft"),          QStringLiteral("rgba(230, 162, 60, 0.12)") },
        { QStringLiteral("warnSoftBorder"),    QStringLiteral("rgba(230, 162, 60, 0.30)") },
        { QStringLiteral("faintSoft"),         QStringLiteral("rgba(152, 164, 179, 0.12)") },
        { QStringLiteral("indigo"),            QStringLiteral("#6366F1") },
        { QStringLiteral("violet"),            QStringLiteral("#8B5CF6") },

        { QStringLiteral("scrollHandle"),      QStringLiteral("#D5DBE3") },
        { QStringLiteral("scrollHandleHover"), QStringLiteral("#BAC3CE") },

        { QStringLiteral("radiusCard"),        QStringLiteral("14px") },
        { QStringLiteral("radiusCtl"),         QStringLiteral("10px") },
        { QStringLiteral("radiusPill"),        QStringLiteral("6px") }
    };
    return table;
}

inline QColor color(const char* name)
{
    const QHash<QString, QString>& table = tokens();
    QHash<QString, QString>::const_iterator found = table.constFind(QString::fromLatin1(name));
    if (found == table.constEnd()) return QColor();
    return QColor(found.value());
}

inline QString loadStyleSheet(const QString& resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();

    QString qss = QString::fromUtf8(file.readAll());
    const QHash<QString, QString>& table = tokens();
    QHash<QString, QString>::const_iterator it = table.constBegin();
    for (; it != table.constEnd(); ++it) {
        qss.replace(QStringLiteral("{{") + it.key() + QStringLiteral("}}"), it.value());
    }
    return qss;
}

inline void setState(QWidget* widget, const char* key, const QString& value)
{
    if (!widget) return;
    widget->setProperty(key, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

inline QPixmap iconPixmap(const QString& resourcePath, const QColor& tint, int px)
{
    static QHash<QString, QPixmap> cache;

    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const QString key = resourcePath + QLatin1Char('|') + tint.name(QColor::HexArgb)
                      + QLatin1Char('|') + QString::number(px)
                      + QLatin1Char('|') + QString::number(dpr);

    QHash<QString, QPixmap>::const_iterator found = cache.constFind(key);
    if (found != cache.constEnd()) return found.value();

    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) return QPixmap();

    QByteArray svg = file.readAll();
    svg.replace("currentColor", "#000000");

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return QPixmap();

    const int side = qMax(1, qRound(px * dpr));
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), tint);
    painter.end();

    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    cache.insert(key, pixmap);
    return pixmap;
}

inline QIcon icon(const QString& resourcePath, const QColor& tint, int px = 18)
{
    const QPixmap pixmap = iconPixmap(resourcePath, tint, px);
    if (pixmap.isNull()) return QIcon();
    return QIcon(pixmap);
}

inline QIcon icon(const QString& resourcePath, const char* tintToken, int px = 18)
{
    return icon(resourcePath, color(tintToken), px);
}

}
}
}
