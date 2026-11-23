// Diagnostic harness (not a pass/fail test): runs Video::process() on every video in $VR_DIAG_DIR,
// the same way a scan does, and prints success/failure, the error, time and memory.
#include "video.h"
#include <QtConcurrent>
#include <QtTest>
#include <bit>
#include <memory>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

class VrDiag : public QObject
{
    Q_OBJECT
  private slots:
    void run()
    {
        const QString dir = qEnvironmentVariable("VR_DIAG_DIR");
        QVERIFY2(!dir.isEmpty(), "set VR_DIAG_DIR");
        const int mode = qEnvironmentVariableIntValue("VR_DIAG_MODE"); // thumbnail mode index; 0 is thumb1
        const int threads = qMax(1, qEnvironmentVariableIntValue("VR_DIAG_THREADS"));
        Prefs prefs;
        prefs.useCacheOption(Prefs::NO_CACHE);
        prefs.thumbnailsMode(); // getter first: the setter ignores the value when nothing was read yet
        prefs.thumbnailsMode(qEnvironmentVariableIsSet("VR_DIAG_MODE") ? mode : cutEnds);
        QStringList files;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            if (!QFileInfo(f).fileName().startsWith('.'))
                files << f;
        }
        files.sort();
        QThreadPool::globalInstance()->setMaxThreadCount(threads);
        QElapsedTimer total;
        total.start();
        QList<QFuture<QString>> futures;
        QList<std::shared_ptr<Video>> videos;
        for (const QString& f : files) {
            auto v = std::make_shared<Video>(prefs, f);
            videos << v;
            futures << QtConcurrent::run([f, v]() {
                QElapsedTimer t;
                t.start();
                const auto r = v->process();
                if (!r.success)
                    v->size = -1; // mark as failed for the similarity table
                return QString("%1 | %2 | %3 ms | %4x%5 | %6")
                    .arg(r.success ? "OK  " : "FAIL", QFileInfo(f).fileName())
                    .arg(t.elapsed())
                    .arg(v->width)
                    .arg(v->height)
                    .arg(r.errorMsg);
            });
        }
        for (auto& fu : futures) {
            fu.waitForFinished();
            printf("%s\n", qPrintable(fu.result()));
        }
        // pHash similarity (matching bits out of 64) for every pair; the app's default 90% is about 58 bits.
        // cutEnds has two segments (start, end): both are shown.
        const int segments = prefs.thumbnailsMode() == cutEnds ? 2 : 1;
        for (int i = 0; i < videos.size(); ++i)
            for (int j = i + 1; j < videos.size(); ++j) {
                if (videos[i]->size < 0 || videos[j]->size < 0)
                    continue;
                QStringList sims;
                for (int s = 0; s < segments; ++s)
                    sims << QString::number(
                        64 - std::popcount(videos[i]->fingerprint(s).phash ^ videos[j]->fingerprint(s).phash));
                printf("SIM %s | %s | %s\n", qPrintable(sims.join('/')),
                       qPrintable(QFileInfo(videos[i]->_filePathName).fileName()),
                       qPrintable(QFileInfo(videos[j]->_filePathName).fileName()));
            }
        long peakMb = -1;
#ifdef Q_OS_UNIX
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        peakMb = ru.ru_maxrss / 1024;
#endif
        printf("TOTAL %lld ms, peak memory %ld MB, threads %d, mode %d\n", total.elapsed(), peakMb, threads,
               prefs.thumbnailsMode());
    }
};
QTEST_MAIN(VrDiag)
#include "tst_vr_diag.moc"
