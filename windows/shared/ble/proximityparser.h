#ifndef PROXIMITYPARSER_H
#define PROXIMITYPARSER_H

#include <QByteArray>
#include <QString>
#include "bleinfo.h"

namespace ProximityParser
{
    AirpodsTrayApp::Enums::AirPodsModel getModelName(quint16 modelId);
    QString getColorName(quint8 colorId);
    QString getConnectionStateName(BleInfo::ConnectionState state);

    /**
     * @brief Parses an Apple Proximity Pairing advertisement payload (manufacturer
     * data for company 0x004C) into a BleInfo structure.
     * @param data Raw manufacturer data, including the 16-byte encrypted tail.
     * @param address Advertiser Bluetooth address (used for RPA verification).
     * @param name Advertised device name, may be empty.
     * @param out Parsed result; only modified when the function returns true.
     * @return true when the payload is a valid, paired-mode AirPods advertisement.
     */
    bool parseProximityAdvertisement(const QByteArray &data, const QString &address,
                                     const QString &name, BleInfo &out);
}

#endif // PROXIMITYPARSER_H
