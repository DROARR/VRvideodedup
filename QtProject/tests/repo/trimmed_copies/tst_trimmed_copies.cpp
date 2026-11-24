#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "../../../app/audiofingerprint.h"
#include "../../../app/comparison/internal/videopairmatcher.h"
#include "../../../app/db.h"
#include "../../../app/prefs.h"
#include "../../../app/trimmedcopyfinder.h"
#include "../../../app/video.h"

#include <memory>

// End to end checks for matching copies at an offset by sound, on the fixtures made by
// samples/videos/trimmed/generate-fixtures.sh. Each "a-" fixture is the same content as a-full.mp4 after a real
// world edit, re-encoded; the negatives are the cases sound alone gets wrong, which the picture check must reject.
namespace
{
const QStringList allFixtures = {
    QStringLiteral("a-full.mp4"),
    QStringLiteral("a-trim-start.mp4"),
    QStringLiteral("a-trim-both.mp4"),
    QStringLiteral("a-intro.mp4"),
    QStringLiteral("a-midcut.mp4"),
    QStringLiteral("a-remaster.mp4"),
    QStringLiteral("f-short-full.mp4"),
    QStringLiteral("f-short-trim.mp4"),
    QStringLiteral("d-same-sound-other-picture.mp4"),
    QStringLiteral("c1-intro-other.mp4"),
    QStringLiteral("c2-intro-other.mp4"),
};
constexpr qint64 FIXTURE_BYTES_BUDGET = 1800000;
constexpr int64_t OFFSET_TOLERANCE_MS = 100; // a few spectrogram frames, well under one video frame of motion

QString fixturesRoot()
{
    QDir candidate(QDir::currentPath());
    while (!candidate.exists(QStringLiteral("samples/videos/trimmed/generate-fixtures.sh")))
        if (!candidate.cdUp())
            return {};
    return candidate.absoluteFilePath(QStringLiteral("samples/videos/trimmed"));
}

QString pairName(const QString& first, const QString& second)
{
    return first < second ? first + QStringLiteral(" <> ") + second : second + QStringLiteral(" <> ") + first;
}
} // namespace

class TestTrimmedCopies : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void test_fixturesAndSizeBudget();
    void test_settingDefaultsOffAndPersists();
    void test_soundIsNotReadWhenDisabled();
    void test_soundAlignsCopiesAtTheirOffsets_data();
    void test_soundAlignsCopiesAtTheirOffsets();
    void test_sharedSoundWithOtherPicturesIsRejected();
    void test_endToEndPairs();
    void test_peaksAreCachedAndReused();

  private:
    std::shared_ptr<Video> processed(const QString& file, Prefs::USE_CACHE_OPTION cacheMode,
                                     const QString& cachePath = QString(), bool detectTrimmed = true) const;
    AudioFingerprint::Alignment alignFixtures(const QString& first, const QString& second) const;
    QString _root;
};

void TestTrimmedCopies::initTestCase()
{
    qSetMessagePattern(QStringLiteral("%{file}(%{line}) %{function}: %{message}"));
    _root = fixturesRoot();
    QVERIFY2(!_root.isEmpty(), "Could not find samples/videos/trimmed");
}

void TestTrimmedCopies::cleanup()
{
    Prefs().resetSettings();
}

std::shared_ptr<Video> TestTrimmedCopies::processed(const QString& file, Prefs::USE_CACHE_OPTION cacheMode,
                                                    const QString& cachePath, bool detectTrimmed) const
{
    Prefs prefs;
    prefs.resetSettings();
    prefs.thumbnailsMode(cutEnds);
    prefs.useCacheOption(cacheMode);
    if (cacheMode != Prefs::NO_CACHE) {
        prefs.cacheFilePathName(cachePath);
        Db::initDbAndCacheLocation(prefs);
    }
    prefs.detectTrimmedCopies(detectTrimmed);
    auto video = std::make_shared<Video>(prefs, QDir(_root).filePath(file));
    const Video::ProcessingResult result = video->process();
    if (!result.success)
        qWarning() << file << "failed:" << result.errorMsg;
    return result.success ? video : nullptr;
}

AudioFingerprint::Alignment TestTrimmedCopies::alignFixtures(const QString& first, const QString& second) const
{
    const auto firstVideo = processed(first, Prefs::NO_CACHE);
    const auto secondVideo = processed(second, Prefs::NO_CACHE);
    if (!firstVideo || !secondVideo)
        return {};
    return AudioFingerprint::align(AudioFingerprint::landmarks(firstVideo->audioPeaks),
                                   AudioFingerprint::landmarks(secondVideo->audioPeaks), firstVideo->duration,
                                   secondVideo->duration);
}

void TestTrimmedCopies::test_fixturesAndSizeBudget()
{
    qint64 total = 0;
    for (const QString& file : allFixtures) {
        const QFileInfo info(QDir(_root).filePath(file));
        QVERIFY2(info.isFile(), qPrintable(QStringLiteral("Missing fixture %1").arg(info.filePath())));
        total += info.size();
    }
    QVERIFY2(
        total <= FIXTURE_BYTES_BUDGET,
        qPrintable(
            QStringLiteral("Trimmed copy fixtures use %1 bytes; budget is %2").arg(total).arg(FIXTURE_BYTES_BUDGET)));
}

void TestTrimmedCopies::test_settingDefaultsOffAndPersists()
{
    Prefs prefs;
    prefs.resetSettings();
    QVERIFY(!prefs.detectTrimmedCopies());
    prefs.detectTrimmedCopies(true);
    QVERIFY(Prefs().detectTrimmedCopies());
    QVERIFY(QSettings(APP_NAME, APP_NAME).value(QStringLiteral("detect_trimmed_copies")).toBool());
}

// Reading sound makes first scans of big files slower, so it must only happen when the user asked for it.
void TestTrimmedCopies::test_soundIsNotReadWhenDisabled()
{
    const auto video = processed(QStringLiteral("a-full.mp4"), Prefs::NO_CACHE, QString(), false);
    QVERIFY(video);
    QVERIFY(!video->audioPeaksAvailable);
    QVERIFY(video->audioPeaks.empty());

    VideoPairMatchConfig config;
    config.thumbnailsMode = cutEnds;
    config.thresholdPhash = 64;
    QVERIFY(config.trimmedCopies == nullptr); // without a search, matching is picture-only as before
}

void TestTrimmedCopies::test_soundAlignsCopiesAtTheirOffsets_data()
{
    QTest::addColumn<QString>("copy");
    QTest::addColumn<qint64>("expectedOffsetMs"); // a-full time = copy time + offset
    QTest::newRow("trimmed start") << QStringLiteral("a-trim-start.mp4") << qint64(8000);
    QTest::newRow("trimmed both ends") << QStringLiteral("a-trim-both.mp4") << qint64(6000);
    QTest::newRow("added intro") << QStringLiteral("a-intro.mp4") << qint64(-6000);
    QTest::newRow("scene cut from the middle") << QStringLiteral("a-midcut.mp4") << qint64(0);
    QTest::newRow("remastered") << QStringLiteral("a-remaster.mp4") << qint64(0);
}

void TestTrimmedCopies::test_soundAlignsCopiesAtTheirOffsets()
{
    QFETCH(QString, copy);
    QFETCH(qint64, expectedOffsetMs);
    const AudioFingerprint::Alignment alignment = alignFixtures(QStringLiteral("a-full.mp4"), copy);
    QVERIFY2(alignment.aligned, qPrintable(QStringLiteral("overlap %1 ms").arg(alignment.overlapMs)));
    QVERIFY2(qAbs(alignment.offsetMs - expectedOffsetMs) <= OFFSET_TOLERANCE_MS,
             qPrintable(QStringLiteral("offset %1 ms, expected %2 ms").arg(alignment.offsetMs).arg(expectedOffsetMs)));
}

// Same soundtrack, different pictures (two phones at one moment, one background track): sound lines them up,
// so only the picture check stands between the user and a wrong suggestion.
void TestTrimmedCopies::test_sharedSoundWithOtherPicturesIsRejected()
{
    const auto full = processed(QStringLiteral("a-full.mp4"), Prefs::NO_CACHE);
    const auto other = processed(QStringLiteral("d-same-sound-other-picture.mp4"), Prefs::NO_CACHE);
    QVERIFY(full && other);
    const AudioFingerprint::Alignment alignment =
        AudioFingerprint::align(AudioFingerprint::landmarks(full->audioPeaks),
                                AudioFingerprint::landmarks(other->audioPeaks), full->duration, other->duration);
    QVERIFY2(alignment.aligned, "the shared soundtrack should align, otherwise this test proves nothing");
    QVERIFY(!TrimmedCopyFinder::confirmPictures(*full, *other, alignment, Prefs()).has_value());

    // And the real copy at an offset passes the same check.
    const auto trimmed = processed(QStringLiteral("a-trim-start.mp4"), Prefs::NO_CACHE);
    QVERIFY(trimmed);
    const AudioFingerprint::Alignment copyAlignment =
        AudioFingerprint::align(AudioFingerprint::landmarks(full->audioPeaks),
                                AudioFingerprint::landmarks(trimmed->audioPeaks), full->duration, trimmed->duration);
    const std::optional<int> similarity = TrimmedCopyFinder::confirmPictures(*full, *trimmed, copyAlignment, Prefs());
    QVERIFY(similarity.has_value());
    QVERIFY(*similarity >= TrimmedCopyFinder::MIN_PICTURE_SIMILARITY);
}

// The whole scan path: every video processed, one library-wide search, then the pair matcher the comparison
// window uses. Every edit of A must be found against the original, the short family clip against its trim, and
// nothing may pair the shared-intro or shared-sound negatives.
void TestTrimmedCopies::test_endToEndPairs()
{
    QVector<Video*> videos;
    std::vector<std::shared_ptr<Video>> owners;
    for (const QString& file : allFixtures) {
        auto video = processed(file, Prefs::NO_CACHE);
        QVERIFY2(video, qPrintable(file));
        QVERIFY2(video->audioPeaksAvailable, qPrintable(QStringLiteral("%1 has no sound peaks").arg(file)));
        videos.append(video.get());
        owners.push_back(std::move(video));
    }

    Prefs prefs;
    TrimmedCopyFinder::Progress progress;
    const auto matches = std::make_shared<const TrimmedCopyMatches>(TrimmedCopyFinder::find(videos, prefs, &progress));
    QCOMPARE(progress.done.load(), progress.total.load());

    VideoPairMatchConfig config;
    config.thumbnailsMode = cutEnds;
    config.thresholdPhash = 64; // strictest picture setting: the offset copies must come from the sound search
    config.sameDurationModifier = 1;
    config.differentDurationModifier = 0;
    config.trimmedCopies = matches;

    const QSet<QString> required = {
        pairName(QStringLiteral("a-full.mp4"), QStringLiteral("a-trim-start.mp4")),
        pairName(QStringLiteral("a-full.mp4"), QStringLiteral("a-trim-both.mp4")),
        pairName(QStringLiteral("a-full.mp4"), QStringLiteral("a-intro.mp4")),
        pairName(QStringLiteral("a-full.mp4"), QStringLiteral("a-midcut.mp4")),
        pairName(QStringLiteral("a-full.mp4"), QStringLiteral("a-remaster.mp4")),
        pairName(QStringLiteral("f-short-full.mp4"), QStringLiteral("f-short-trim.mp4")),
    };
    QSet<QString> found;
    QStringList forbidden;
    for (int left = 0; left < videos.size(); ++left)
        for (int right = left + 1; right < videos.size(); ++right) {
            const VideoPairMatchResult result = VideoPairMatcher::match(*videos[left], *videos[right], config);
            if (!result.matches)
                continue;
            const QString name = pairName(allFixtures[left], allFixtures[right]);
            found.insert(name);
            // Two edits of A are the same content too (e.g. trimmed against remastered); anything else is wrong.
            const bool bothFromA = allFixtures[left].startsWith(QStringLiteral("a-"))
                                   && allFixtures[right].startsWith(QStringLiteral("a-"));
            // Picture matching keeps its own contract (cutEnds can land in a shared intro, as with c1/c2): only
            // pairs this feature adds are held to it here.
            if (result.trimmedCopy && !bothFromA && !required.contains(name))
                forbidden.append(name);
            if (required.contains(name) && result.trimmedCopy) {
                // The offset is reported for the order asked: left time = right time + offset.
                const auto reverse = matches->find(videos[right]->_filePathName, videos[left]->_filePathName);
                QVERIFY(reverse.has_value());
                QCOMPARE(reverse->offsetMs, -result.trimmedCopy->offsetMs);
            }
        }
    QVERIFY2(forbidden.isEmpty(), qPrintable(QStringLiteral("Wrong pairs: %1").arg(forbidden.join(", "))));
    const QSet<QString> missing = required - found;
    QVERIFY2(
        missing.isEmpty(),
        qPrintable(QStringLiteral("Missed pairs: %1").arg(QStringList(missing.begin(), missing.end()).join(", "))));
}

void TestTrimmedCopies::test_peaksAreCachedAndReused()
{
    QTemporaryDir cacheDir;
    QVERIFY(cacheDir.isValid());
    const QString cachePath = cacheDir.filePath(QStringLiteral("cache.db"));

    const auto first = processed(QStringLiteral("f-short-trim.mp4"), Prefs::WITH_CACHE, cachePath);
    QVERIFY(first && first->audioPeaksAvailable);

    // CACHE_ONLY never decodes, so equal peaks prove they came from the cache.
    const auto cached = processed(QStringLiteral("f-short-trim.mp4"), Prefs::CACHE_ONLY, cachePath);
    QVERIFY(cached && cached->audioPeaksAvailable);
    QVERIFY(cached->audioPeaks == first->audioPeaks);

    // Emptying the cache from the menu must also forget sound.
    Prefs prefs;
    prefs.cacheFilePathName(cachePath);
    QVERIFY(Db::emptyAllDb(prefs));
    std::vector<uint32_t> afterEmpty;
    QVERIFY(!Db(cachePath).readAudioPeaks(QDir(_root).filePath(QStringLiteral("f-short-trim.mp4")), afterEmpty));
}

QTEST_MAIN(TestTrimmedCopies)
#include "tst_trimmed_copies.moc"
