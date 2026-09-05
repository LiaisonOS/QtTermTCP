/*
 * Bulletin.h
 *
 * BBS bulletin metadata and store helpers for QtTermTCP.
 *
 * Author : Sylvain Deguire (VA2OPS)
 *
 * The store is rooted at ~/.config/liaisonos/bulletins/ with three lifecycle
 * subdirectories — pending/, sent/, failed/. Each bulletin is two files
 * sharing the same id stem: <id>.json (metadata) and <id>.txt (body, plain
 * text, LF line endings). All writes are atomic (temp file + rename) so a
 * crash never leaves a half-written JSON. QtTermTCP only writes to pending/;
 * the daemon (li-automation, via QtTermTCP --send-bulletin in Phase 2) is the
 * only mover between pending/ and sent/.
 */

#ifndef BULLETIN_H
#define BULLETIN_H

#include <QString>
#include <QDateTime>
#include <QList>

struct Bulletin
{
    QString id;
    QString type;
    QString to;
    QString title;
    QString atBbs;
    QString haRoute;
    QString status;
    QDateTime created;
    QDateTime sentAt;
    QString bid;
    QString messageNumber;
    QString lastError;
    int retryCount = 0;
    QString scheduledMissionId;

    // MD5-based "already sent" tracking. After a successful send,
    // BulletinSender writes the MD5 of the TXT body into last_sent_md5
    // and stores the timestamp + BID. The daemon's pre-fire check compares
    // last_sent_md5 to the CURRENT TXT MD5 — if they match, the mission
    // silently no-ops (content unchanged since last send). If they differ
    // (operator edited or external script regenerated the TXT), the mission
    // fires. Files never move out of pending/.
    QString lastSentMd5;
    QString lastSentBid;
    QString lastSentAt;
    int     sendCount = 0;

    QString body;
};

class BulletinStore
{
public:
    static QString rootDir();
    static QString pendingDir();
    static QString sentDir();
    static QString failedDir();

    static bool ensureDirs(QString *errorOut = nullptr);

    static QString generateId(const QString &title);

    static bool save(const Bulletin &b, QString *errorOut = nullptr);

    static bool load(const QString &id, const QString &subDir, Bulletin *out,
                     QString *errorOut = nullptr);

    static bool remove(const QString &id, const QString &subDir,
                       QString *errorOut = nullptr);

    static QList<Bulletin> list(const QString &subDir);
};

#endif
