#include <QApplication>
#include <QGuiApplication>
#include <QObject>
#include <QSystemTrayIcon>
#include <QWindow>

#include "enums.h"

class QMenu;
class QAction;
class QActionGroup;

class TrayIconManager : public QObject
{
    Q_OBJECT

public:
    explicit TrayIconManager(QObject *parent = nullptr);

    void updateBatteryStatus(const QString &status);

    void updateNoiseControlState(AirpodsTrayApp::Enums::NoiseControlMode);

    void updateConversationalAwareness(bool enabled);

    // Capability gate — hides the CA menu action on models without it.
    void setConversationalAwarenessSupported(bool supported);

    void updateEarStatus(bool leftInEar, bool rightInEar, bool leftInCase, bool rightInCase,
                         const QString &podIconFile, const QString &caseIconFile, bool isHeadset);

    void resetTrayIcon()
    {
        const QIcon icon(QStringLiteral(":/icons/assets/airpods.png"));
        setAppAndTrayIcon(icon);
        trayIcon->setToolTip("");
    }

private slots:
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);

private:
    QSystemTrayIcon *trayIcon;
    QMenu *trayMenu;
    QAction *caToggleAction;
    QActionGroup *noiseControlGroup;

    void setupMenuActions();

    // qApp->setWindowIcon only updates the app *default* — existing windows
    // (the frameless QML ApplicationWindow) never pick it up on Windows, so
    // the taskbar button stayed on the startup icon. Setting the icon on each
    // QWindow issues WM_SETICON and actually refreshes the taskbar button.
    void setAppAndTrayIcon(const QIcon &icon)
    {
        trayIcon->setIcon(icon);
        qApp->setWindowIcon(icon);
        const auto windows = qApp->allWindows();
        for (QWindow *w : windows)
            w->setIcon(icon);
    }

    void updateIconFromBattery(const QString &status);
    void renderIcon();
    int m_leftLevel = 0;
    int m_rightLevel = 0;
    bool m_leftInEar = false;
    bool m_rightInEar = false;
    bool m_leftInCase = false;
    bool m_rightInCase = false;
    bool m_headset = false;
    QString m_podIconFile;
    QString m_caseIconFile;

signals:
    void trayClicked();
    void noiseControlChanged(AirpodsTrayApp::Enums::NoiseControlMode);
    void conversationalAwarenessToggled(bool enabled);
    void openApp();
    void openSettings();
};