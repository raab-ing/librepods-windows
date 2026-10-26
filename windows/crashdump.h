#ifndef CRASHDUMP_H
#define CRASHDUMP_H

// Installs process-wide diagnostics:
//  - qInstallMessageHandler writing every Qt log message (the LOG_* macros
//    are qCDebug/qCInfo/etc.) to librepods.log beside the executable,
//    flushed per line so the last lines before a crash actually land.
//  - An unhandled-exception filter writing a minidump to
//    librepods-crash-<timestamp>.dmp beside the executable.
//
// Call once, early in main(). Safe to call before QApplication exists.

namespace CrashDump
{
    void install();
}

#endif // CRASHDUMP_H
