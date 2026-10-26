#ifndef HEADORIENTATION_H
#define HEADORIENTATION_H

#include <QList>
#include <QVector>
#include <QtMath>
#include <algorithm>

// Calibrated head orientation from the AACP 0x17 stream.
//
// The o-fields at packet offsets 43/45/47 form a ~unit-magnitude
// direction vector in the pod's own frame. The frame rotates with pod
// seating, so NO fixed readout basis survives re-seating — the readout
// axes are learned online per seating:
//
//   * The accel channels (offsets 51/53) carry motion bursts whose
//     DIRECTION in (accH,accV) space identifies the motion type
//     (verified on labeled captures, both pods, two seatings):
//       yaw burst   ~ ( 1.0,  0.55)   sign(accH) = turn direction
//       pitch burst ~ ( 0.5, -0.85)   sign(accV) = nod direction, but
//                                     mirrored per pod (L:+, R:-)
//       roll burst  ~ ( 1.0,  1.0)    same-sign comparable magnitude
//   * During a classified burst the direction the o-vector moves is by
//     definition that motion's displacement axis in the CURRENT pod
//     frame. Summed over bursts (signed by the accel convention), the
//     axes converge — no coherence-flip: the accel sign already carries
//     the direction, and flipping on early garbage latches bad axes.
//   * Readout: displacement dotted onto each learned axis, unmixed by
//     the 2x2 Gram matrix when the axes are well separated.
//   * Hemisphere flips — the stream occasionally emits the antipodal
//     representation of the same orientation (v -> -v). Each sample is
//     folded toward the previous one for a continuous trajectory.
//   * Startup garbage — the first frames after START are ramp/noise;
//     they are discarded before the neutral baseline is calibrated.
class HeadOrientation
{
public:
    // podSign: +1 when the left pod streams, -1 for the right pod — the
    // accV anatomical sign is mirrored between the two pods.
    void feed(int o1, int o2, int o3, int accH, int accV, int podSign)
    {
        if (podSign != 0 && m_podSign != 0 && podSign != m_podSign)
        {
            // Primary pod switched mid-session — the learned axes are
            // in the other pod's frame. Relearn.
            reset();
        }
        if (podSign != 0)
            m_podSign = podSign;

        double v[3] = {o1 / 32767.0, o2 / 32767.0, o3 / 32767.0};
        if (m_havePrev && dot(v, m_prev) < 0.0)
        {
            v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2];
        }
        const double dv[3] = {v[0] - m_prev[0], v[1] - m_prev[1],
                              v[2] - m_prev[2]};
        m_prev[0] = v[0]; m_prev[1] = v[1]; m_prev[2] = v[2];
        m_havePrev = true;

        if (!m_calibrated)
        {
            if (m_skipped < kStartupDiscard)
            {
                ++m_skipped;
                return;
            }
            if (qAbs(o1) > kSaturation || qAbs(o2) > kSaturation || qAbs(o3) > kSaturation)
                return;
            // Only calibrate while the head is still — a mid-motion
            // stream start must not poison the neutral baseline.
            if (qAbs(accH) > kStillThresh || qAbs(accV) > kStillThresh)
                return;
            m_samples.append({v[0], v[1], v[2]});
            if (m_samples.size() >= kCalibrationSamples)
            {
                QList<double> c1, c2, c3;
                for (const auto &s : m_samples) { c1 << s[0]; c2 << s[1]; c3 << s[2]; }
                auto median = [](QList<double> x) {
                    std::sort(x.begin(), x.end());
                    return x.at(x.size() / 2);
                };
                m_neutral[0] = median(c1);
                m_neutral[1] = median(c2);
                m_neutral[2] = median(c3);
                m_calibrated = true;
            }
            return;
        }

        learnAxes(dv, accH, accV);

        const double d[3] = {v[0] - m_neutral[0], v[1] - m_neutral[1],
                             v[2] - m_neutral[2]};
        double ry = dot(d, m_axisYaw), rp = dot(d, m_axisPitch);
        const double g = dot(m_axisYaw, m_axisPitch);
        const double det = 1.0 - g * g;
        if (det > 0.12)
        {
            const double uy = ry, up = rp;
            ry = (uy - g * up) / det;
            rp = (up - g * uy) / det;
        }
        double yaw = m_haveYaw ? kGain * ry : 0.0;
        double pitch = m_havePitch ? kGain * rp : 0.0;
        if (qAbs(pitch) > 120.0 || qAbs(yaw) > 120.0)
            return; // garbage burst — keep last good pose
        m_pitch = pitch;
        m_yaw = yaw;
        m_valid = true;
    }

    bool calibrated() const { return m_calibrated; }
    bool valid() const { return m_valid; }
    double pitch() const { return m_pitch; }
    double yaw() const { return m_yaw; }

    // Diagnostics for the dev pose line.
    bool haveYaw() const { return m_haveYaw; }
    bool havePitch() const { return m_havePitch; }
    int yawBursts() const { return m_yawBursts; }
    int pitchBursts() const { return m_pitchBursts; }

    void reset()
    {
        m_samples.clear();
        m_skipped = 0;
        m_calibrated = false;
        m_valid = false;
        m_havePrev = false;
        m_pitch = m_yaw = 0.0;
        m_haveYaw = m_havePitch = false;
        m_axisYaw[0] = m_axisYaw[1] = m_axisYaw[2] = 0.0;
        m_axisPitch[0] = m_axisPitch[1] = m_axisPitch[2] = 0.0;
        m_yawBursts = m_pitchBursts = 0;
    }

private:
    // Classify a motion burst by the direction of its accel reading.
    // Yaw-classified samples vote for the yaw axis (signed by accH);
    // pitch-classified for the pitch axis (signed by accV, mirrored on
    // the right pod); roll samples are ignored — they carry no usable
    // yaw/pitch information and only pollute the estimates.
    void learnAxes(const double dv[3], int accH, int accV)
    {
        const double dvn = norm(dv);
        const double am = std::sqrt(static_cast<double>(accH) * accH +
                                    static_cast<double>(accV) * accV);
        if (dvn < kMinBurstDv || am < kMinBurstAcc)
            return;
        const double ah = accH / am, av = accV / am;
        const double dy = qAbs(ah * kSigYawH + av * kSigYawV);
        const double dp = qAbs(ah * kSigPitchH + av * kSigPitchV);
        const double dr = qAbs(ah * kSigRollH + av * kSigRollV);
        if (dy >= dp && dy >= dr)
        {
            if (dy < kSigMatch)
                return;
            const double s = accH > 0 ? 1.0 : -1.0;
            m_axisYaw[0] += s * dv[0] / dvn;
            m_axisYaw[1] += s * dv[1] / dvn;
            m_axisYaw[2] += s * dv[2] / dvn;
            if (++m_yawBursts >= kAxisLearnSamples && !m_haveYaw)
            {
                normalize(m_axisYaw);
                m_haveYaw = true;
            }
            else if (m_haveYaw)
                normalize(m_axisYaw);
        }
        else if (dp >= dr)
        {
            if (dp < kSigMatch)
                return;
            const double s = (accV > 0 ? 1.0 : -1.0) * m_podSign;
            m_axisPitch[0] += s * dv[0] / dvn;
            m_axisPitch[1] += s * dv[1] / dvn;
            m_axisPitch[2] += s * dv[2] / dvn;
            if (++m_pitchBursts >= kAxisLearnSamples && !m_havePitch)
            {
                normalize(m_axisPitch);
                m_havePitch = true;
            }
            else if (m_havePitch)
                normalize(m_axisPitch);
        }
    }

    static double dot(const double a[3], const double b[3])
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    static double norm(const double a[3])
    {
        const double n = std::sqrt(dot(a, a));
        return n < 1e-9 ? 1.0 : n;
    }
    static void normalize(double a[3])
    {
        const double n = norm(a);
        a[0] /= n; a[1] /= n; a[2] /= n;
    }

    static constexpr int kCalibrationSamples = 10;
    static constexpr int kStartupDiscard = 25;
    static constexpr int kSaturation = 31000;
    static constexpr int kStillThresh = 25;
    static constexpr int kAxisLearnSamples = 8;
    static constexpr double kMinBurstAcc = 40.0;
    static constexpr double kMinBurstDv = 0.004;
    static constexpr double kSigMatch = 0.75;
    // Accel burst signatures in (accH,accV) space, unit-normalized —
    // measured on labeled captures for both pods.
    static constexpr double kSigYawH = 0.876, kSigYawV = 0.481;
    static constexpr double kSigPitchH = 0.507, kSigPitchV = -0.862;
    static constexpr double kSigRollH = 0.707, kSigRollV = 0.707;
    // Degrees per unit-length displacement of the direction vector.
    static constexpr double kGain = 250.0;

    QList<QVector<double>> m_samples;
    int m_skipped = 0;
    int m_podSign = 0;
    double m_neutral[3] = {0.0, 0.0, 0.0};
    double m_prev[3] = {0.0, 0.0, 0.0};
    bool m_havePrev = false;
    bool m_calibrated = false;
    bool m_valid = false;
    double m_pitch = 0.0, m_yaw = 0.0;

    double m_axisYaw[3] = {0.0, 0.0, 0.0};
    double m_axisPitch[3] = {0.0, 0.0, 0.0};
    int m_yawBursts = 0, m_pitchBursts = 0;
    bool m_haveYaw = false, m_havePitch = false;
};

#endif // HEADORIENTATION_H
