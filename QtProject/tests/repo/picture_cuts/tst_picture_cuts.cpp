#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "../../../app/db.h"
#include "../../../app/prefs.h"
#include "../../../app/trimmedcopyfinder.h"
#include "../../../app/video.h"
#include "../../../app/visualtimeline.h"

#include <memory>

// End to end checks for finding clips cut from a longer video by picture, when their sound differs, on the
// fixtures made by samples/videos/picture-cuts/generate-fixtures.sh.
namespace
{
const QStringList pictureFixtures = {
    QStringLiteral("p-full.mp4"),
    QStringLiteral("p-cut-reencoded.mp4"),
    QStringLiteral("p-cut-lossless.mp4"),
    QStringLiteral("p-cut-remaster.mp4"),
    QStringLiteral("p-two-parts.mp4"),
    QStringLiteral("n-other-region.mp4"),
    QStringLiteral("n-same-set-other-action.mp4"),
    QStringLiteral("n-same-set-another-action.mp4"),
};
constexpr qint64 FIXTURE_BYTES_BUDGET = 900000;
// Steady footage looks alike for a while around each moment, so a picture alignment is only frame exact to within
// about the time the picture takes to change visibly.
constexpr int64_t OFFSET_TOLERANCE_MS = 1500;

QString samplesRoot()
{
    QDir candidate(QDir::currentPath());
    while (!candidate.exists(QStringLiteral("samples/videos/picture-cuts/generate-fixtures.sh")))
        if (!candidate.cdUp())
            return {};
    return candidate.absoluteFilePath(QStringLiteral("samples/videos"));
}
} // namespace

class TestPictureCuts : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void test_fixturesAndSizeBudget();
    void test_settingDefaultsOffAndPersists();
    void test_timelineIsNotReadWhenDisabled();
    void test_timelineSamplesKeyframes();
    void test_cutsWithOtherSoundAreFound();
    void test_lookAlikeRoomIsRejectedFrameByFrame();
    void test_soundFixturesGainNoWrongPairs();
    void test_timelineIsCachedAndReused();

  private:
    std::shared_ptr<Video> processed(const QString& path, Prefs::USE_CACHE_OPTION cacheMode = Prefs::NO_CACHE,
                                     const QString& cachePath = QString(), bool byPicture = true,
                                     bool bySound = false) const;
    QString _root;
};

void TestPictureCuts::initTestCase()
{
    qSetMessagePattern(QStringLiteral("%{file}(%{line}) %{function}: %{message}"));
    _root = samplesRoot();
    QVERIFY2(!_root.isEmpty(), "Could not find samples/videos/picture-cuts");
}

void TestPictureCuts::cleanup()
{
    Prefs().resetSettings();
}

std::shared_ptr<Video> TestPictureCuts::processed(const QString& path, Prefs::USE_CACHE_OPTION cacheMode,
                                                  const QString& cachePath, bool byPicture, bool bySound) const
{
    Prefs prefs;
    prefs.resetSettings();
    prefs.thumbnailsMode(cutEnds);
    prefs.useCacheOption(cacheMode);
    if (cacheMode != Prefs::NO_CACHE) {
        prefs.cacheFilePathName(cachePath);
        Db::initDbAndCacheLocation(prefs);
    }
    prefs.detectCutCopiesByPicture(byPicture);
    prefs.detectTrimmedCopies(bySound);
    auto video = std::make_shared<Video>(prefs, path);
    const Video::ProcessingResult result = video->process();
    if (!result.success)
        qWarning() << path << "failed:" << result.errorMsg;
    return result.success ? video : nullptr;
}

void TestPictureCuts::test_fixturesAndSizeBudget()
{
    qint64 total = 0;
    for (const QString& file : pictureFixtures) {
        const QFileInfo info(QDir(_root).filePath(QStringLiteral("picture-cuts/") + file));
        QVERIFY2(info.isFile(), qPrintable(QStringLiteral("Missing fixture %1").arg(info.filePath())));
        total += info.size();
    }
    QVERIFY2(
        total <= FIXTURE_BYTES_BUDGET,
        qPrintable(
            QStringLiteral("Picture cut fixtures use %1 bytes; budget is %2").arg(total).arg(FIXTURE_BYTES_BUDGET)));
}

void TestPictureCuts::test_settingDefaultsOffAndPersists()
{
    Prefs prefs;
    prefs.resetSettings();
    QVERIFY(!prefs.detectCutCopiesByPicture());
    prefs.detectCutCopiesByPicture(true);
    QVERIFY(Prefs().detectCutCopiesByPicture());
    QVERIFY(QSettings(APP_NAME, APP_NAME).value(QStringLiteral("detect_cut_copies_by_picture")).toBool());
}

// Decoding keyframes of every video is the slowest part of a scan with this on, so it must only happen when asked.
void TestPictureCuts::test_timelineIsNotReadWhenDisabled()
{
    const auto video =
        processed(QDir(_root).filePath(QStringLiteral("picture-cuts/p-full.mp4")), Prefs::NO_CACHE, QString(), false);
    QVERIFY(video);
    QVERIFY(video->visualTimeline.empty());
}

void TestPictureCuts::test_timelineSamplesKeyframes()
{
    const VisualTimeline::ExtractionResult timeline =
        VisualTimeline::extract(QDir(_root).filePath(QStringLiteral("picture-cuts/p-full.mp4")));
    QVERIFY2(timeline.error.isEmpty(), qPrintable(timeline.error));
    // p-full.mp4 is 60 s with a keyframe every 5 s: one sample per keyframe, in time order.
    QCOMPARE(timeline.samples.size(), size_t(12));
    for (size_t i = 1; i < timeline.samples.size(); ++i)
        QVERIFY(timeline.samples[i].timeMs > timeline.samples[i - 1].timeMs);
}

// The whole path with sound matching off: every clip cut from p-full.mp4 has other sound than it, so any match
// must come from pictures. Each must be found against the source at its cut position, and nothing may pair the
// look-alike negatives.
void TestPictureCuts::test_cutsWithOtherSoundAreFound()
{
    QVector<Video*> videos;
    std::vector<std::shared_ptr<Video>> owners;
    for (const QString& file : pictureFixtures) {
        auto video = processed(QDir(_root).filePath(QStringLiteral("picture-cuts/") + file));
        QVERIFY2(video, qPrintable(file));
        QVERIFY2(!video->visualTimeline.empty(), qPrintable(QStringLiteral("%1 has no picture timeline").arg(file)));
        videos.append(video.get());
        owners.push_back(std::move(video));
    }

    TrimmedCopyFinder::Progress progress;
    const TrimmedCopyMatches matches = TrimmedCopyFinder::find(videos, Prefs(), &progress);
    QCOMPARE(progress.done.load(), progress.total.load());

    struct Expected {
        QString clip;
        QList<int64_t> offsetsMs; // where the clip starts in p-full.mp4 (two-parts: either of its parts)
    };
    const QList<Expected> expected = {
        {QStringLiteral("p-cut-reencoded.mp4"), {12000}},
        {QStringLiteral("p-cut-lossless.mp4"), {20000}},
        {QStringLiteral("p-cut-remaster.mp4"), {30000}},
        {QStringLiteral("p-two-parts.mp4"), {5000, 20000}},
    };
    const QString full = videos[0]->_filePathName;
    for (const Expected& clip : expected) {
        const QString clipPath = QDir(_root).filePath(QStringLiteral("picture-cuts/") + clip.clip);
        const auto match = matches.find(full, clipPath);
        QVERIFY2(match.has_value(), qPrintable(QStringLiteral("Missed %1").arg(clip.clip)));
        QVERIFY(match->byPicture);
        bool nearExpected = false;
        for (const int64_t offset : clip.offsetsMs)
            nearExpected = nearExpected || qAbs(match->offsetMs - offset) <= OFFSET_TOLERANCE_MS;
        QVERIFY2(nearExpected,
                 qPrintable(QStringLiteral("%1 found at offset %2 ms").arg(clip.clip).arg(match->offsetMs)));
    }

    for (int left = 0; left < videos.size(); ++left)
        for (int right = left + 1; right < videos.size(); ++right) {
            const bool negative = pictureFixtures[left].startsWith(QStringLiteral("n-"))
                                  || pictureFixtures[right].startsWith(QStringLiteral("n-"));
            if (negative)
                QVERIFY2(
                    !matches.find(videos[left]->_filePathName, videos[right]->_filePathName).has_value(),
                    qPrintable(
                        QStringLiteral("Wrong pair: %1 <> %2").arg(pictureFixtures[left], pictureFixtures[right])));
        }
}

// Two scenes filmed in one room look alike at every moment, so their keyframes do line up; the frame by frame check
// must then reject them, because no single offset makes several moments of one match the other.
void TestPictureCuts::test_lookAlikeRoomIsRejectedFrameByFrame()
{
    const QString first = QDir(_root).filePath(QStringLiteral("picture-cuts/n-same-set-other-action.mp4"));
    const QString second = QDir(_root).filePath(QStringLiteral("picture-cuts/n-same-set-another-action.mp4"));
    const auto firstVideo = processed(first);
    const auto secondVideo = processed(second);
    QVERIFY(firstVideo && secondVideo);
    const VisualTimeline::Alignment alignment = VisualTimeline::align(
        firstVideo->visualTimeline, secondVideo->visualTimeline, firstVideo->duration, secondVideo->duration);
    QVERIFY2(alignment.aligned, "the look-alike scenes should line up by keyframes, otherwise this proves nothing");
    QVERIFY(!TrimmedCopyFinder::confirmTimeline(first, second, alignment).has_value());

    // And the real cut copy passes the same check.
    const QString full = QDir(_root).filePath(QStringLiteral("picture-cuts/p-full.mp4"));
    const QString clip = QDir(_root).filePath(QStringLiteral("picture-cuts/p-cut-remaster.mp4"));
    const auto fullVideo = processed(full);
    const auto clipVideo = processed(clip);
    QVERIFY(fullVideo && clipVideo);
    const VisualTimeline::Alignment copyAlignment = VisualTimeline::align(
        fullVideo->visualTimeline, clipVideo->visualTimeline, fullVideo->duration, clipVideo->duration);
    QVERIFY(copyAlignment.aligned);
    QVERIFY(TrimmedCopyFinder::confirmTimeline(full, clip, copyAlignment).has_value());
}

// Turning the picture pass on next to sound must not invent pairs among the sound fixtures: the two videos sharing
// an intro, and the video sharing a whole soundtrack with another picture, stay unmatched.
void TestPictureCuts::test_soundFixturesGainNoWrongPairs()
{
    const QDir soundDir(QDir(_root).filePath(QStringLiteral("trimmed")));
    QVector<Video*> videos;
    std::vector<std::shared_ptr<Video>> owners;
    for (const QString& file : soundDir.entryList({QStringLiteral("*.mp4")}, QDir::Files, QDir::Name)) {
        auto video = processed(soundDir.filePath(file), Prefs::NO_CACHE, QString(), true, true);
        QVERIFY2(video, qPrintable(file));
        videos.append(video.get());
        owners.push_back(std::move(video));
    }
    const TrimmedCopyMatches matches = TrimmedCopyFinder::find(videos, Prefs());
    for (int left = 0; left < videos.size(); ++left)
        for (int right = left + 1; right < videos.size(); ++right) {
            const QString l = QFileInfo(videos[left]->_filePathName).fileName();
            const QString r = QFileInfo(videos[right]->_filePathName).fileName();
            const bool sameContent = (l.startsWith(QStringLiteral("a-")) && r.startsWith(QStringLiteral("a-")))
                                     || (l.startsWith(QStringLiteral("f-")) && r.startsWith(QStringLiteral("f-")));
            if (!sameContent)
                QVERIFY2(!matches.find(videos[left]->_filePathName, videos[right]->_filePathName).has_value(),
                         qPrintable(QStringLiteral("Wrong pair: %1 <> %2").arg(l, r)));
        }
}

void TestPictureCuts::test_timelineIsCachedAndReused()
{
    QTemporaryDir cacheDir;
    QVERIFY(cacheDir.isValid());
    const QString cachePath = cacheDir.filePath(QStringLiteral("cache.db"));
    const QString path = QDir(_root).filePath(QStringLiteral("picture-cuts/p-cut-lossless.mp4"));

    const auto first = processed(path, Prefs::WITH_CACHE, cachePath);
    QVERIFY(first && !first->visualTimeline.empty());
    // CACHE_ONLY never decodes, so an equal timeline proves it came from the cache.
    const auto cached = processed(path, Prefs::CACHE_ONLY, cachePath);
    QVERIFY(cached);
    QCOMPARE(cached->visualTimeline.size(), first->visualTimeline.size());
    for (size_t i = 0; i < first->visualTimeline.size(); ++i) {
        QCOMPARE(cached->visualTimeline[i].timeMs, first->visualTimeline[i].timeMs);
        QCOMPARE(cached->visualTimeline[i].phash, first->visualTimeline[i].phash);
    }

    Prefs prefs;
    prefs.cacheFilePathName(cachePath);
    QVERIFY(Db::emptyAllDb(prefs));
    std::vector<VisualTimeline::Sample> afterEmpty;
    QVERIFY(!Db(cachePath).readVisualTimeline(path, afterEmpty));
}

QTEST_MAIN(TestPictureCuts)
#include "tst_picture_cuts.moc"
