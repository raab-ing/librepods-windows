#ifndef GESTUREDETECTOR_H
#define GESTUREDETECTOR_H

#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QDateTime>
#include <QtMath>
#include <cmath>
#include <utility>

// Nod/shake head-gesture detector — port of the Android app's
// utils/GestureDetector.kt. Fed by the head-tracking acceleration axes
// (AACP 0x17 packet offsets 51/53). A deliberate nod ("yes") or shake
// ("no") emits gestureAccepted / gestureRejected; ordinary head movement
// must not trigger either — the confidence score requires amplitude,
// rhythm, sign alternation, and axis isolation.
class GestureDetector : public QObject
{
    Q_OBJECT
public:
    explicit GestureDetector(QObject *parent = nullptr) : QObject(parent)
    {
        m_evalTimer.setInterval(50); // mirrors Android's 50 ms eval cadence
        connect(&m_evalTimer, &QTimer::timeout, this, [this]() {
            const int result = evaluate();
            if (result > 0)
            {
                reset();
                emit gestureAccepted();
            }
            else if (result < 0)
            {
                reset();
                emit gestureRejected();
            }
        });
    }

    bool running() const { return m_running; }

public slots:
    void start()
    {
        if (m_running)
            return;
        m_running = true;
        reset();
        m_evalTimer.start();
    }

    void stop()
    {
        m_running = false;
        m_evalTimer.stop();
        reset();
    }

    void feed(int horizontal, int vertical)
    {
        if (!m_running)
            return;

        // Ignore out-of-range samples (calibration junk per Android).
        if (qAbs(horizontal) > 6000 || qAbs(vertical) > 6000)
            return;

        // 3-sample moving-average smoothing.
        const double h = smooth(horizontal, m_hAvg);
        const double v = smooth(vertical, m_vAvg);
        m_hBuffer.enqueue(h);
        m_vBuffer.enqueue(v);
        if (m_hBuffer.size() > 100)
            m_hBuffer.dequeue();
        if (m_vBuffer.size() > 100)
            m_vBuffer.dequeue();
        trackExtremes(m_hBuffer, m_hIncreasing, m_hExtremes);
        trackExtremes(m_vBuffer, m_vIncreasing, m_vExtremes);
    }

signals:
    void gestureAccepted(); // vertical oscillation — "yes" nod
    void gestureRejected(); // horizontal oscillation — "no" shake

private:
    struct Extreme
    {
        int index;
        double value;
        qint64 timestamp;
    };

    static double smooth(double value, QQueue<double> &avg)
    {
        avg.enqueue(value);
        if (avg.size() > 3)
            avg.dequeue();
        double sum = 0;
        for (double v : std::as_const(avg))
            sum += v;
        return sum / avg.size();
    }

    static double variance(const QList<double> &values)
    {
        if (values.size() <= 1)
            return 0.0;
        double mean = 0;
        for (double v : values)
            mean += v;
        mean /= values.size();
        double var = 0;
        for (double v : values)
            var += (v - mean) * (v - mean);
        return var / values.size();
    }

    // Track peaks and troughs on one axis. A direction reversal past a
    // dynamic threshold registers the previous sample as an extreme if its
    // amplitude clears the minimum.
    void trackExtremes(const QQueue<double> &buffer, bool &increasing, QList<Extreme> &extremes)
    {
        if (buffer.size() < 4)
            return;

        const QList<double> tail = buffer.mid(buffer.size() - 4);
        const double current = buffer.last();
        const double prev = buffer.at(buffer.size() - 2);
        const double dynThreshold = qMax(50.0, qMin(150.0, variance(tail) / 3.0));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        if (increasing && current < prev - dynThreshold)
        {
            if (qAbs(prev) > kPeakThreshold)
            {
                extremes.append({static_cast<int>(buffer.size()) - 1, prev, now});
                if (m_lastExtremeTime > 0)
                {
                    m_peakIntervals.enqueue((now - m_lastExtremeTime) / 1000.0);
                    if (m_peakIntervals.size() > 5)
                        m_peakIntervals.dequeue();
                    m_speedIntervals.enqueue(now - m_lastExtremeTime);
                    if (m_speedIntervals.size() > 5)
                        m_speedIntervals.dequeue();
                }
                m_lastExtremeTime = now;
                if (extremes.size() > 16)
                    extremes.removeFirst();
            }
            increasing = false;
        }
        else if (!increasing && current > prev + dynThreshold)
        {
            if (qAbs(prev) > kPeakThreshold)
            {
                extremes.append({static_cast<int>(buffer.size()) - 1, prev, now});
                if (m_lastExtremeTime > 0)
                {
                    m_peakIntervals.enqueue((now - m_lastExtremeTime) / 1000.0);
                    if (m_peakIntervals.size() > 5)
                        m_peakIntervals.dequeue();
                    m_speedIntervals.enqueue(now - m_lastExtremeTime);
                    if (m_speedIntervals.size() > 5)
                        m_speedIntervals.dequeue();
                }
                m_lastExtremeTime = now;
                if (extremes.size() > 16)
                    extremes.removeFirst();
            }
            increasing = true;
        }
    }

    int requiredExtremes() const
    {
        if (m_speedIntervals.isEmpty())
            return 3;
        double avg = 0;
        for (qint64 i : std::as_const(m_speedIntervals))
            avg += i;
        avg /= m_speedIntervals.size();
        return avg < 300.0 ? 4 : 3; // faster movement needs one more extreme
    }

    double rhythmConsistency() const
    {
        if (m_peakIntervals.size() < 2)
            return 0.0;
        double mean = 0;
        for (double i : std::as_const(m_peakIntervals))
            mean += i;
        mean /= m_peakIntervals.size();
        if (mean <= 0)
            return 0.0;
        double var = 0;
        for (double i : std::as_const(m_peakIntervals))
            var += std::pow(i / mean - 1.0, 2);
        var /= m_peakIntervals.size();
        return qMax(0.0, 1.0 - qMin(1.0, var / 0.5));
    }

    double confidence(const QList<Extreme> &extremes, const QQueue<double> &thisBuf,
                      const QQueue<double> &otherBuf, int required) const
    {
        if (extremes.size() < required)
            return 0.0;

        const QList<Extreme> recent = extremes.mid(extremes.size() - required);
        double avgAmp = 0;
        for (const auto &e : recent)
            avgAmp += qAbs(e.value);
        avgAmp /= recent.size();
        const double amplitude = qMin(1.0, avgAmp / 600.0);

        const double rhythm = rhythmConsistency();

        bool alternating = true;
        for (int i = 1; i < recent.size(); ++i)
            if ((recent.at(i).value > 0) == (recent.at(i - 1).value > 0))
                alternating = false;
        const double alternation = alternating ? 1.0 : 0.5;

        double otherAmp = 0;
        int otherCount = 0;
        const int take = recent.size() * 2;
        for (int i = qMax(0, otherBuf.size() - take); i < otherBuf.size(); ++i)
        {
            otherAmp += qAbs(otherBuf.at(i));
            ++otherCount;
        }
        otherAmp = otherCount ? otherAmp / otherCount : 0.0;
        const double isolation = qMin(1.0, avgAmp / (otherAmp + 0.1) * 1.2);
        Q_UNUSED(thisBuf);

        return amplitude * 0.4 + rhythm * 0.2 + alternation * 0.2 + isolation * 0.2;
    }

    int evaluate()
    {
        const int required = requiredExtremes();
        if (confidence(m_vExtremes, m_vBuffer, m_hBuffer, required) >= 0.7)
            return 1;
        if (confidence(m_hExtremes, m_hBuffer, m_vBuffer, required) >= 0.7)
            return -1;
        return 0;
    }

    void reset()
    {
        m_hBuffer.clear();
        m_vBuffer.clear();
        m_hAvg.clear();
        m_vAvg.clear();
        m_hExtremes.clear();
        m_vExtremes.clear();
        m_peakIntervals.clear();
        m_speedIntervals.clear();
        m_hIncreasing = m_vIncreasing = false;
        m_lastExtremeTime = 0;
    }

    static constexpr double kPeakThreshold = 400.0;

    bool m_running = false;
    QTimer m_evalTimer;
    QQueue<double> m_hBuffer, m_vBuffer, m_hAvg, m_vAvg;
    QList<Extreme> m_hExtremes, m_vExtremes;
    QQueue<double> m_peakIntervals;
    QQueue<qint64> m_speedIntervals;
    bool m_hIncreasing = false, m_vIncreasing = false;
    qint64 m_lastExtremeTime = 0;
};

#endif // GESTUREDETECTOR_H
