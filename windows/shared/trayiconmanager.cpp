#include "trayiconmanager.h"

#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QPainter>
#include <QFont>
#include <QColor>
#include <QActionGroup>

using namespace AirpodsTrayApp::Enums;

TrayIconManager::TrayIconManager(QObject *parent) : QObject(parent)
{
    // Initialize tray icon
    trayIcon = new QSystemTrayIcon(QIcon(":/icons/assets/airpods.png"), this);
    trayMenu = new QMenu();

    // Setup basic menu actions
    setupMenuActions();

    // Connect signals
    trayIcon->setContextMenu(trayMenu);
    connect(trayIcon, &QSystemTrayIcon::activated, this, &TrayIconManager::onTrayIconActivated);

    trayIcon->show();
}

void TrayIconManager::TrayIconManager::updateBatteryStatus(const QString &status)
{
    trayIcon->setToolTip(status.isEmpty() ? tr("LibrePods — not connected")
                                        : tr("Battery Status: ") + status);
    updateIconFromBattery(status);
}

void TrayIconManager::updateNoiseControlState(NoiseControlMode mode)
{
    QList<QAction *> actions = noiseControlGroup->actions();
    for (QAction *action : actions)
    {
        action->setChecked(action->data().toInt() == (int)mode);
    }
}

void TrayIconManager::updateConversationalAwareness(bool enabled)
{
    caToggleAction->setChecked(enabled);
}

void TrayIconManager::setConversationalAwarenessSupported(bool supported)
{
    caToggleAction->setVisible(supported);
}

void TrayIconManager::setupMenuActions()
{
    // Open action
    QAction *openAction = new QAction(tr("Open"), trayMenu);
    trayMenu->addAction(openAction);
    connect(openAction, &QAction::triggered, qApp, [this](){emit openApp();});

    // Settings Menu

    QAction *settingsMenu = new QAction(tr("Settings"), trayMenu);
    trayMenu->addAction(settingsMenu);
    connect(settingsMenu, &QAction::triggered, qApp, [this](){emit openSettings();});

    trayMenu->addSeparator();

    // Conversational Awareness Toggle
    caToggleAction = new QAction(tr("Conversational Awareness"), trayMenu);
    caToggleAction->setCheckable(true);
    trayMenu->addAction(caToggleAction);
    caToggleAction->setVisible(false); // shown once device capabilities arrive
    connect(caToggleAction, &QAction::triggered, this, [this](bool checked)
            { emit conversationalAwarenessToggled(checked); });

    trayMenu->addSeparator();

    // Noise Control Options
    noiseControlGroup = new QActionGroup(trayMenu);
    const QPair<QString, NoiseControlMode> noiseOptions[] = {
        {tr("Adaptive"), NoiseControlMode::Adaptive},
        {tr("Transparency"), NoiseControlMode::Transparency},
        {tr("Noise Cancellation"), NoiseControlMode::NoiseCancellation},
        {tr("Off"), NoiseControlMode::Off}};

    for (auto option : noiseOptions)
    {
        QAction *action = new QAction(option.first, trayMenu);
        action->setCheckable(true);
        action->setData((int)option.second);
        noiseControlGroup->addAction(action);
        trayMenu->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode = option.second]()
                { emit noiseControlChanged(mode); });
    }

    trayMenu->addSeparator();

    // Quit action
    QAction *quitAction = new QAction(tr("Quit"), trayMenu);
    trayMenu->addAction(quitAction);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
}

void TrayIconManager::updateEarStatus(bool leftInEar, bool rightInEar, bool leftInCase, bool rightInCase,
                                      const QString &podIconFile, const QString &caseIconFile, bool isHeadset)
{
    m_leftInEar = leftInEar;
    m_rightInEar = rightInEar;
    m_leftInCase = leftInCase;
    m_rightInCase = rightInCase;
    m_headset = isHeadset;
    m_podIconFile = podIconFile;
    m_caseIconFile = caseIconFile;
    renderIcon();
}

void TrayIconManager::updateIconFromBattery(const QString &status)
{
    int leftLevel = 0;
    int rightLevel = 0;

    if (!status.isEmpty())
    {
        // Parse the battery status string
        QStringList parts = status.split(", ");
        if (parts.size() >= 2) {
            leftLevel = parts[0].split(": ")[1].replace("%", "").toInt();
            rightLevel = parts[1].split(": ")[1].replace("%", "").toInt();
        } else if (parts.size() == 1) {
            leftLevel = parts[0].split(": ")[1].replace("%", "").toInt();
        }
    }

    m_leftLevel = leftLevel;
    m_rightLevel = rightLevel;
    renderIcon();
}

static QColor levelColor(int level)
{
    return level > 50 ? QColor("#30D158")
         : level > 20 ? QColor("#FFD60A")
                      : QColor("#FF453A");
}

void TrayIconManager::renderIcon()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QString podPath = ":/icons/assets/" + (m_podIconFile.isEmpty() ? QStringLiteral("airpods.png") : m_podIconFile);
    const QString casePath = ":/icons/assets/" + (m_caseIconFile.isEmpty() ? QStringLiteral("airpods.png") : m_caseIconFile);
    QImage podImg(podPath);

    const bool leftVisible = !m_leftInCase;
    const bool rightVisible = !m_rightInCase;

    if (!m_headset && !podImg.isNull() && (leftVisible || rightVisible))
    {
        // A docked pod's artwork hides entirely (matches the main window);
        // a present-but-out-of-ear pod stays visible but dimmed.
        if (leftVisible && rightVisible)
        {
            painter.setOpacity(m_leftInEar ? 1.0 : 0.30);
            painter.drawImage(QRectF(1, 3, 14, 26), podImg);
            painter.setOpacity(m_rightInEar ? 1.0 : 0.30);
            painter.drawImage(QRectF(17, 3, 14, 26), podImg.mirrored(true, false));
        }
        else
        {
            const bool left = leftVisible;
            painter.setOpacity((left ? m_leftInEar : m_rightInEar) ? 1.0 : 0.30);
            painter.drawImage(QRectF(9, 3, 14, 26), left ? podImg : podImg.mirrored(true, false));
        }

        // Battery pill under each visible pod, colored by that pod's level.
        const auto pill = [&](int level, int cx) {
            if (level <= 0) return;
            const QColor c = levelColor(level);
            painter.setOpacity(1.0);
            painter.setBrush(c);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(cx - 9, 24, 18, 8, 4, 4);
            painter.setPen(level > 20 ? Qt::black : Qt::white);
            QFont f("Segoe UI", 6, QFont::Bold);
            painter.setFont(f);
            painter.drawText(QRect(cx - 9, 23, 18, 9), Qt::AlignCenter, QString::number(level));
        };
        if (leftVisible && rightVisible)
        {
            pill(m_leftLevel, 8);
            pill(m_rightLevel, 24);
        }
        else
        {
            pill(leftVisible ? m_leftLevel : m_rightLevel, 16);
        }
    }
    else if (!m_headset && !leftVisible && !rightVisible)
    {
        // Everything docked: case artwork, dimmed so it reads as idle.
        QImage caseImg(casePath);
        if (!caseImg.isNull())
        {
            painter.setOpacity(0.75);
            painter.drawImage(QRectF(4, 4, 24, 24), caseImg);
        }
    }
    else
    {
        // Headset / no pod info: keep the original centered number badge.
        const int minLevel = (m_leftLevel == 0) ? m_rightLevel
                           : (m_rightLevel == 0) ? m_leftLevel
                                                 : qMin(m_leftLevel, m_rightLevel);
        painter.setBrush(levelColor(minLevel));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(1, 1, 30, 30, 9, 9);
        painter.setPen(minLevel > 20 ? Qt::black : Qt::white);
        painter.setFont(QFont("Segoe UI", 13, QFont::Bold));
        painter.drawText(pixmap.rect(), Qt::AlignCenter, QString::number(minLevel));
    }
    painter.end();

    // Mirror onto the window icon so the taskbar button shows the same live state.
    setAppAndTrayIcon(QIcon(pixmap));
}

void TrayIconManager::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger)
    {
        emit trayClicked();
    }
}

