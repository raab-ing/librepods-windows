#ifndef SYSTEMSLEEPMONITOR_HPP
#define SYSTEMSLEEPMONITOR_HPP

#include <QObject>
#include <QCoreApplication>
#include <QAbstractNativeEventFilter>
#include <QDebug>

#include <windows.h>

// Windows sleep/wake monitor listening for WM_POWERBROADCAST via Qt's native
// event filter, replacing the logind PrepareForSleep D-Bus signal.
class SystemSleepMonitor : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit SystemSleepMonitor(QObject *parent = nullptr) : QObject(parent)
    {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }

    ~SystemSleepMonitor() override
    {
        if (QCoreApplication::instance())
        {
            QCoreApplication::instance()->removeNativeEventFilter(this);
        }
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(result);
        if (eventType != "windows_generic_MSG")
        {
            return false;
        }

        auto *msg = static_cast<MSG *>(message);
        if (msg->message != WM_POWERBROADCAST)
        {
            return false;
        }

        switch (msg->wParam)
        {
        case PBT_APMSUSPEND:
            emit systemGoingToSleep();
            break;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
            emit systemWakingUp();
            break;
        default:
            break;
        }
        return false;
    }

signals:
    void systemGoingToSleep();
    void systemWakingUp();
};

#endif // SYSTEMSLEEPMONITOR_HPP
