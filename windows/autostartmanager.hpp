#ifndef AUTOSTARTMANAGER_HPP
#define AUTOSTARTMANAGER_HPP

#include <QObject>
#include <QSettings>
#include <QCoreApplication>
#include <QDir>

// Windows autostart via HKCU\...\Run registry key, replacing the Linux
// .desktop autostart file.
class AutoStartManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool autoStartEnabled READ autoStartEnabled WRITE setAutoStartEnabled NOTIFY autoStartEnabledChanged)

public:
    explicit AutoStartManager(QObject *parent = nullptr) : QObject(parent)
    {
    }

    bool autoStartEnabled() const
    {
        QSettings runKey(runKeyPath(), QSettings::NativeFormat);
        return runKey.contains(QCoreApplication::applicationName());
    }

    void setAutoStartEnabled(bool enabled)
    {
        if (autoStartEnabled() == enabled)
        {
            return;
        }

        QSettings runKey(runKeyPath(), QSettings::NativeFormat);
        if (enabled)
        {
            QString appPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
            runKey.setValue(QCoreApplication::applicationName(),
                            QStringLiteral("\"%1\" --hide").arg(appPath));
        }
        else
        {
            runKey.remove(QCoreApplication::applicationName());
        }

        emit autoStartEnabledChanged(enabled);
    }

private:
    static QString runKeyPath()
    {
        return QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    }

signals:
    void autoStartEnabledChanged(bool enabled);
};

#endif // AUTOSTARTMANAGER_HPP
