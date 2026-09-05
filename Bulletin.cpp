/*
 * Bulletin.cpp
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#include "Bulletin.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>


static QString configRoot()
{
    QString h = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return h + "/.config/liaisonos";
}

QString BulletinStore::rootDir()    { return configRoot() + "/bulletins"; }
QString BulletinStore::pendingDir() { return rootDir() + "/pending"; }
QString BulletinStore::sentDir()    { return rootDir() + "/sent"; }
QString BulletinStore::failedDir()  { return rootDir() + "/failed"; }


bool BulletinStore::ensureDirs(QString *errorOut)
{
    QDir d;
    for (const QString &p : { pendingDir(), sentDir(), failedDir() }) {
        if (!d.mkpath(p)) {
            if (errorOut) *errorOut = QString("cannot create %1").arg(p);
            return false;
        }
    }
    return true;
}


// Slug derivation keeps the id stem readable in a directory listing without
// leaking PII or punctuation that some filesystems hate. We collapse the
// title to lowercase ASCII-safe tokens, cap to 40 chars, and append a
// 6-digit time tail to prevent same-title collisions within a second.
QString BulletinStore::generateId(const QString &title)
{
    QString slug = title.toLower();
    slug.replace(QRegularExpression("[^a-z0-9]+"), "-");
    slug = slug.mid(0, 40);
    while (slug.endsWith('-')) slug.chop(1);
    while (slug.startsWith('-')) slug.remove(0, 1);
    if (slug.isEmpty()) slug = "bulletin";

    QDateTime now = QDateTime::currentDateTime();
    QString date = now.toString("yyyyMMdd");
    QString tail = now.toString("HHmmss");
    return QString("%1-%2-%3").arg(date, slug, tail);
}


static QJsonObject toJson(const Bulletin &b)
{
    QJsonObject o;
    o["id"]       = b.id;
    o["type"]     = b.type;
    o["to"]       = b.to;
    o["title"]    = b.title;
    o["at_bbs"]   = b.atBbs;
    o["ha_route"] = b.haRoute;
    o["status"]   = b.status;
    o["body_file"] = b.id + ".txt";
    if (b.created.isValid())
        o["created"] = b.created.toString(Qt::ISODate);
    if (b.sentAt.isValid())
        o["sent_at"] = b.sentAt.toString(Qt::ISODate);
    if (!b.bid.isEmpty())
        o["bid"] = b.bid;
    if (!b.messageNumber.isEmpty())
        o["message_number"] = b.messageNumber;
    if (!b.lastError.isEmpty())
        o["last_error"] = b.lastError;
    if (!b.lastSentMd5.isEmpty())
        o["last_sent_md5"] = b.lastSentMd5;
    if (!b.lastSentBid.isEmpty())
        o["last_sent_bid"] = b.lastSentBid;
    if (!b.lastSentAt.isEmpty())
        o["last_sent_at"] = b.lastSentAt;
    if (b.sendCount > 0)
        o["send_count"] = b.sendCount;
    if (b.retryCount > 0)
        o["retry_count"] = b.retryCount;
    if (!b.scheduledMissionId.isEmpty())
        o["scheduled_mission_id"] = b.scheduledMissionId;
    return o;
}


static void fromJson(const QJsonObject &o, Bulletin *b)
{
    b->id        = o.value("id").toString();
    b->type      = o.value("type").toString();
    b->to        = o.value("to").toString();
    // Read "title" (BBS-native term). Accept legacy "subject" as a fallback
    // so any test bulletins composed before this rename still load cleanly.
    b->title     = o.value("title").toString();
    if (b->title.isEmpty())
        b->title = o.value("subject").toString();
    b->atBbs     = o.value("at_bbs").toString();
    b->haRoute   = o.value("ha_route").toString();
    b->status    = o.value("status").toString();
    b->bid       = o.value("bid").toString();
    b->messageNumber = o.value("message_number").toString();
    b->lastError = o.value("last_error").toString();
    b->lastSentMd5 = o.value("last_sent_md5").toString();
    b->lastSentBid = o.value("last_sent_bid").toString();
    b->lastSentAt  = o.value("last_sent_at").toString();
    b->sendCount   = o.value("send_count").toInt(0);
    b->retryCount = o.value("retry_count").toInt(0);
    b->scheduledMissionId = o.value("scheduled_mission_id").toString();

    if (o.contains("created"))
        b->created = QDateTime::fromString(o.value("created").toString(), Qt::ISODate);
    if (o.contains("sent_at"))
        b->sentAt = QDateTime::fromString(o.value("sent_at").toString(), Qt::ISODate);
}


bool BulletinStore::save(const Bulletin &b, QString *errorOut)
{
    if (!ensureDirs(errorOut))
        return false;
    if (b.id.isEmpty()) {
        if (errorOut) *errorOut = "bulletin id is empty";
        return false;
    }

    QString dir = pendingDir();
    QString jsonPath = dir + "/" + b.id + ".json";
    QString bodyPath = dir + "/" + b.id + ".txt";

    // The .txt format is the human-readable form the operator sees in a
    // text editor: line 1 is the title, the rest is the body. Match the
    // BBS conversation order — title first, then body, just like the BBS
    // prompts you for them. The body trailing newline keeps line counts
    // predictable when the operator concatenates messages.
    QSaveFile body(bodyPath);
    if (!body.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = "cannot open body file: " + body.errorString();
        return false;
    }
    QString combined = b.title;
    combined.append('\n');
    combined.append(b.body);
    if (!combined.endsWith('\n')) combined.append('\n');
    body.write(combined.toUtf8());
    if (!body.commit()) {
        if (errorOut) *errorOut = "body commit failed: " + body.errorString();
        return false;
    }

    QSaveFile json(jsonPath);
    if (!json.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = "cannot open metadata file: " + json.errorString();
        return false;
    }
    QJsonDocument doc(toJson(b));
    json.write(doc.toJson(QJsonDocument::Indented));
    if (!json.commit()) {
        if (errorOut) *errorOut = "metadata commit failed: " + json.errorString();
        return false;
    }
    return true;
}


bool BulletinStore::load(const QString &id, const QString &subDir,
                         Bulletin *out, QString *errorOut)
{
    if (!out) return false;
    QString dir = rootDir() + "/" + subDir;
    QString jsonPath = dir + "/" + id + ".json";
    QString bodyPath = dir + "/" + id + ".txt";

    QFile json(jsonPath);
    if (!json.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = "cannot open " + jsonPath;
        return false;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (errorOut) *errorOut = "JSON parse: " + err.errorString();
        return false;
    }
    fromJson(doc.object(), out);

    QFile body(bodyPath);
    if (body.open(QIODevice::ReadOnly)) {
        // .txt is "title\n<body>". Split on the first newline; treat what
        // follows as the body. If the operator edited the .txt directly, the
        // file's title line is the source of truth and overrides whatever
        // happened to be cached in .json.
        QString raw = QString::fromUtf8(body.readAll());
        int nl = raw.indexOf('\n');
        if (nl < 0) {
            out->title = raw.trimmed();
            out->body  = QString();
        } else {
            out->title = raw.left(nl).trimmed();
            out->body  = raw.mid(nl + 1);
            if (out->body.endsWith('\n'))
                out->body.chop(1);
        }
    }

    return true;
}


bool BulletinStore::remove(const QString &id, const QString &subDir,
                           QString *errorOut)
{
    QString dir = rootDir() + "/" + subDir;
    bool a = QFile::remove(dir + "/" + id + ".json");
    bool b = QFile::remove(dir + "/" + id + ".txt");
    if (!a && !b) {
        if (errorOut) *errorOut = "nothing removed for id " + id;
        return false;
    }
    return true;
}


QList<Bulletin> BulletinStore::list(const QString &subDir)
{
    QList<Bulletin> out;
    QDir d(rootDir() + "/" + subDir);
    if (!d.exists()) return out;
    QStringList jsons = d.entryList(QStringList() << "*.json",
                                    QDir::Files, QDir::Time | QDir::Reversed);
    for (const QString &name : jsons) {
        Bulletin b;
        QString id = name;
        if (id.endsWith(".json")) id.chop(5);
        if (load(id, subDir, &b))
            out.append(b);
    }
    return out;
}
