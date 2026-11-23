// Regression tests for scan failures seen on large VR libraries: files that video players open fine
// (corrupt packets, no seek index / unknown duration) used to fail, and 8K captures used gigabytes of memory.
#include <QtTest>

#include "../../../app/prefs.h"
#include "../../../app/video.h"

#include <bit>
#include <memory>

namespace
{
constexpr int MATCH_THRESHOLD_BITS = 58; // approximately the 90% UI setting

QString projectRoot()
{
    QDir candidate(QDir::currentPath());
    while (!candidate.exists(QStringLiteral("samples/videos/matching-ground-truth.csv")))
        if (!candidate.cdUp())
            return {};
    return candidate.absolutePath();
}

int phashSimilarity(const Video& left, const Video& right, int segment)
{
    return 64 - std::popcount(left.fingerprint(segment).phash ^ right.fingerprint(segment).phash);
}
} // namespace

class TestRepoVrRobustness : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void cleanup();
    void test_corruptPacketsAreSkipped();
    void test_unknownDurationWithoutSeekIndex();
    void test_highResolutionCapturesAreDownscaled();

  private:
    Prefs prefs(int thumbnailMode) const;
    QString _videos;
    QTemporaryDir _tmp;
};

void TestRepoVrRobustness::initTestCase()
{
    const QString root = projectRoot();
    QVERIFY2(!root.isEmpty(), "Could not find the repository root");
    _videos = QDir(root).filePath(QStringLiteral("samples/videos"));
    QVERIFY(_tmp.isValid());
}

void TestRepoVrRobustness::init()
{
    Prefs().resetSettings();
}

void TestRepoVrRobustness::cleanup()
{
    Prefs().resetSettings();
}

Prefs TestRepoVrRobustness::prefs(int thumbnailMode) const
{
    Prefs p;
    p.useCacheOption(Prefs::NO_CACHE);
    p.thumbnailsMode(); // the setter only stores the value once the preference was read
    p.thumbnailsMode(thumbnailMode);
    return p;
}

void TestRepoVrRobustness::test_corruptPacketsAreSkipped()
{
    const QString source = QDir(_videos).filePath(QStringLiteral("Nice_720p_1000kbps.mp4"));
    const QString corrupt = _tmp.filePath(QStringLiteral("nice-corrupt-packets.mp4"));
    QFile::remove(corrupt);
    QVERIFY(QFile::copy(source, corrupt));
    QFile file(corrupt);
    QVERIFY(file.open(QIODevice::ReadWrite));
    const qint64 size = file.size();
    QByteArray garbage(6000, Qt::Uninitialized);
    for (int i = 0; i < garbage.size(); ++i)
        garbage[i] = static_cast<char>((i * 37 + 11) % 256);
    for (const double fraction : {0.25, 0.5, 0.75, 0.9}) {
        QVERIFY(file.seek(static_cast<qint64>(size * fraction)));
        QCOMPARE(file.write(garbage), garbage.size());
    }
    file.close();

    for (const int mode : {static_cast<int>(cutEnds), static_cast<int>(thumb4)}) {
        const Prefs p = prefs(mode);
        Video original(p, source);
        Video damaged(p, corrupt);
        const auto originalResult = original.process();
        const auto damagedResult = damaged.process();
        QVERIFY2(originalResult.success, qPrintable(originalResult.errorMsg));
        QVERIFY2(damagedResult.success, qPrintable(damagedResult.errorMsg));
        const int segments = mode == cutEnds ? 2 : 1;
        for (int segment = 0; segment < segments; ++segment)
            QVERIFY2(phashSimilarity(original, damaged, segment) >= MATCH_THRESHOLD_BITS,
                     qPrintable(QStringLiteral("mode %1 segment %2 similarity %3")
                                    .arg(mode)
                                    .arg(segment)
                                    .arg(phashSimilarity(original, damaged, segment))));
    }
}

void TestRepoVrRobustness::test_unknownDurationWithoutSeekIndex()
{
    const QString source = QDir(_videos).filePath(QStringLiteral("Nice_383p_500kbps.mp4"));
    const QString remux = QDir(_videos).filePath(QStringLiteral("robustness/nice-no-seek-index.mkv"));
    for (const int mode : {static_cast<int>(cutEnds), static_cast<int>(thumb4)}) {
        const Prefs p = prefs(mode);
        Video original(p, source);
        Video noIndex(p, remux);
        const auto originalResult = original.process();
        const auto noIndexResult = noIndex.process();
        QVERIFY2(originalResult.success, qPrintable(originalResult.errorMsg));
        QVERIFY2(noIndexResult.success, qPrintable(noIndexResult.errorMsg));
        // The estimate overshoots; reaching the end must correct it to the real length.
        QVERIFY2(qAbs(noIndex.duration - original.duration) < 1000,
                 qPrintable(QStringLiteral("learned %1 ms, real %2 ms").arg(noIndex.duration).arg(original.duration)));
        const int segments = mode == cutEnds ? 2 : 1;
        for (int segment = 0; segment < segments; ++segment)
            QVERIFY2(phashSimilarity(original, noIndex, segment) >= MATCH_THRESHOLD_BITS,
                     qPrintable(QStringLiteral("mode %1 segment %2 similarity %3")
                                    .arg(mode)
                                    .arg(segment)
                                    .arg(phashSimilarity(original, noIndex, segment))));
    }
}

void TestRepoVrRobustness::test_highResolutionCapturesAreDownscaled()
{
    const QString video8k = QDir(_videos).filePath(QStringLiteral("robustness/vr-8k-tiny.mp4"));
    const Prefs p = prefs(thumb12);
    Video video(p, video8k);
    const auto result = video.process();
    QVERIFY2(result.success, qPrintable(result.errorMsg));
    // Metadata keeps the real size for display and sorting.
    QCOMPARE(video.width, static_cast<short>(7680));
    QCOMPARE(video.height, static_cast<short>(3840));

    const QImage capture = video.ffmpegLib_captureAt(50);
    QVERIFY(!capture.isNull());
    QCOMPARE(capture.size(), QSize(1920, 960));
}

QTEST_MAIN(TestRepoVrRobustness)
#include "tst_vr_robustness.moc"
