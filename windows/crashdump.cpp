#include "crashdump.h"

#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QTextStream>

#include <cstdlib>
#include <exception>

#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

namespace
{

QFile g_logFile;
QMutex g_logMutex;
QString g_appDir;
QtMessageHandler g_previousHandler = nullptr;

QString appDirPath()
{
    if (g_appDir.isEmpty())
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (len > 0)
        {
            QString full = QString::fromWCharArray(path, len);
            g_appDir = full.left(full.lastIndexOf('\\'));
        }
    }
    return g_appDir;
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QMutexLocker locker(&g_logMutex);
    if (g_logFile.fileName().isEmpty())
    {
        const QString path = appDirPath() + QStringLiteral("/librepods.log");
        g_logFile.setFileName(path);
        // Keep the log bounded: start a fresh file when it would exceed 5 MB.
        if (QFile::exists(path) && QFile(path).size() > 5 * 1024 * 1024)
            QFile::remove(path);
        g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    if (g_logFile.isOpen())
    {
        QTextStream stream(&g_logFile);
        const char *level = type == QtDebugMsg ? "D" : type == QtInfoMsg ? "I"
                          : type == QtWarningMsg ? "W" : type == QtCriticalMsg ? "E" : "F";
        stream << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << ' '
               << level << ' ' << message << '\n';
        stream.flush();
        g_logFile.flush(); // flush per line — a crash must not take the tail of the log with it
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *info)
{
    const QString path = appDirPath() + QStringLiteral("/librepods-crash-%1.dmp")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));

    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo{};
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ExceptionPointers = info;
        dumpInfo.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MiniDumpNormal, &dumpInfo, nullptr, nullptr);
        CloseHandle(file);
    }

    if (g_logFile.isOpen())
    {
        QMutexLocker locker(&g_logMutex);
        QTextStream stream(&g_logFile);
        stream << "F Unhandled exception 0x"
               << QString::number(info->ExceptionRecord->ExceptionCode, 16)
               << " — dump written to " << path << '\n';
        stream.flush();
        g_logFile.flush();
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// std::terminate (uncaught C++ exceptions — e.g. winrt::hresult_error
// escaping a WinRT callback thread) bypasses the SEH filter entirely,
// which used to leave neither a dump nor a log line. Capture it too:
// the dump has no exception record but the thread stacks survive.
[[noreturn]] void terminateHandler()
{
    const QString path = appDirPath() + QStringLiteral("/librepods-crash-%1.dmp")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));

    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MiniDumpNormal, nullptr, nullptr, nullptr);
        CloseHandle(file);
    }

    if (g_logFile.isOpen())
    {
        QMutexLocker locker(&g_logMutex);
        QTextStream stream(&g_logFile);
        stream << "F std::terminate — dump written to " << path << '\n';
        stream.flush();
        g_logFile.flush();
    }

    std::abort();
}

} // namespace

void CrashDump::install()
{
    g_previousHandler = qInstallMessageHandler(messageHandler);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(terminateHandler);
}
