#include "BBSDirectory.h"
#include <QNetworkRequest>
#include <QSet>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QXmlStreamReader>
#include <QSqlQuery>
#include <QSqlError>
#include <cmath>

BBSDirectory *g_bbsDirectory = nullptr;

BBSDirectory::BBSDirectory(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &BBSDirectory::onNetworkReply);
    initDb();
}

QString BBSDirectory::localFilePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                  + "/QtTermTCP/bbs-directory";
    QDir().mkpath(dir);
    return dir + "/bbs-directory.db";
}

QString BBSDirectory::jsonFilePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                  + "/QtTermTCP/bbs-directory";
    return dir + "/bbs-directory.json";
}

void BBSDirectory::initDb()
{
    m_db = QSqlDatabase::addDatabase("QSQLITE", "bbsdirectory");
    m_db.setDatabaseName(localFilePath());
    if (!m_db.open()) {
        qWarning() << "BBSDirectory: cannot open DB:" << m_db.lastError().text();
        return;
    }

    QSqlQuery q(m_db);
    q.exec("PRAGMA foreign_keys = ON");
    q.exec("CREATE TABLE IF NOT EXISTS stations ("
           " id         INTEGER PRIMARY KEY AUTOINCREMENT,"
           " callsign   TEXT UNIQUE NOT NULL,"
           " sysop      TEXT,"
           " network    TEXT,"
           " location   TEXT,"
           " grid       TEXT,"
           " power      TEXT,"
           " antenna    TEXT,"
           " notes      TEXT,"
           " local_only INTEGER NOT NULL DEFAULT 0"
           ")");
    q.exec("CREATE TABLE IF NOT EXISTS bands ("
           " id         INTEGER PRIMARY KEY AUTOINCREMENT,"
           " station_id INTEGER NOT NULL REFERENCES stations(id) ON DELETE CASCADE,"
           " band       TEXT,"
           " frequency  INTEGER,"
           " mode       TEXT,"
           " hours      TEXT,"
           " bandwidth  TEXT"
           ")");

    // One-time migration from old JSON file
    QSqlQuery cnt(m_db);
    cnt.exec("SELECT COUNT(*) FROM stations");
    if (cnt.next() && cnt.value(0).toInt() == 0) {
        QString oldJson = jsonFilePath();
        if (QFile::exists(oldJson)) {
            qDebug() << "BBSDirectory: migrating from JSON to SQLite";
            QFile f(oldJson);
            if (f.open(QIODevice::ReadOnly)) {
                parseJson(f.readAll());   // fills m_stations
                f.close();
                for (const BBSStation &st : m_stations)
                    writeStationToDb(st);
                QFile::rename(oldJson, oldJson + ".bak");
            }
        }
    }

    reloadFromDb();
}

void BBSDirectory::reloadFromDb()
{
    m_stations.clear();
    if (!m_db.isOpen()) return;

    QSqlQuery sq(m_db);
    sq.exec("SELECT id, callsign, sysop, network, location, grid, power, antenna, notes, local_only FROM stations ORDER BY callsign");
    while (sq.next()) {
        BBSStation st;
        int sid        = sq.value(0).toInt();
        st.callsign    = sq.value(1).toString();
        st.sysop       = sq.value(2).toString();
        st.network     = sq.value(3).toString();
        st.location    = sq.value(4).toString();
        st.grid        = sq.value(5).toString();
        st.power       = sq.value(6).toString();
        st.antenna     = sq.value(7).toString();
        st.notes       = sq.value(8).toString();
        st.localOnly   = sq.value(9).toInt() != 0;

        QSqlQuery bq(m_db);
        bq.prepare("SELECT band, frequency, mode, hours, bandwidth FROM bands WHERE station_id = ?");
        bq.addBindValue(sid);
        bq.exec();
        while (bq.next()) {
            BBSBandEntry be;
            be.band      = bq.value(0).toString();
            be.frequency = (quint64)bq.value(1).toLongLong();
            be.mode      = bq.value(2).toString();
            be.hours     = bq.value(3).toString();
            QString bwStr = bq.value(4).toString();
            for (const QString &s : bwStr.split(',', Qt::SkipEmptyParts))
                be.bandwidth.append(s.trimmed().toInt());
            st.bands.append(be);
        }
        m_stations.append(st);
    }
}

void BBSDirectory::writeStationToDb(const BBSStation &station)
{
    if (!m_db.isOpen()) return;

    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO stations "
              "(callsign, sysop, network, location, grid, power, antenna, notes, local_only) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(station.callsign);
    q.addBindValue(station.sysop);
    q.addBindValue(station.network);
    q.addBindValue(station.location);
    q.addBindValue(station.grid);
    q.addBindValue(station.power);
    q.addBindValue(station.antenna);
    q.addBindValue(station.notes);
    q.addBindValue(station.localOnly ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "BBSDirectory: insert station failed:" << q.lastError().text();
        return;
    }

    // Get the station id (may be from REPLACE)
    QSqlQuery idq(m_db);
    idq.prepare("SELECT id FROM stations WHERE callsign = ?");
    idq.addBindValue(station.callsign);
    idq.exec();
    if (!idq.next()) return;
    int sid = idq.value(0).toInt();

    // Replace band rows
    QSqlQuery del(m_db);
    del.prepare("DELETE FROM bands WHERE station_id = ?");
    del.addBindValue(sid);
    del.exec();

    for (const BBSBandEntry &be : station.bands) {
        QStringList bwParts;
        for (int bw : be.bandwidth) bwParts << QString::number(bw);
        QSqlQuery bq(m_db);
        bq.prepare("INSERT INTO bands (station_id, band, frequency, mode, hours, bandwidth) VALUES (?,?,?,?,?,?)");
        bq.addBindValue(sid);
        bq.addBindValue(be.band);
        bq.addBindValue((qlonglong)be.frequency);
        bq.addBindValue(be.mode);
        bq.addBindValue(be.hours);
        bq.addBindValue(bwParts.join(','));
        bq.exec();
    }
}

void BBSDirectory::loadFromFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qDebug() << "BBSDirectory: cannot open" << path;
        return;
    }
    parseJson(f.readAll());
    emit loaded();
}

void BBSDirectory::fetchFromUrl(const QString &url)
{
    QUrl qurl(url);
    QNetworkRequest req(qurl);
    req.setAttribute(QNetworkRequest::User, QString("json"));
    m_nam->get(req);
}

void BBSDirectory::fetchHFConditions()
{
    QUrl qurl("https://www.hamqsl.com/solarxml.php");
    QNetworkRequest req(qurl);
    req.setAttribute(QNetworkRequest::User, QString("hamqsl"));
    m_nam->get(req);
}

void BBSDirectory::onNetworkReply(QNetworkReply *reply)
{
    reply->deleteLater();
    QString type = reply->request().attribute(QNetworkRequest::User).toString();

    if (reply->error() != QNetworkReply::NoError) {
        if (type != "hamqsl")
            emit fetchError(reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();

    if (type == "hamqsl") {
        // Parse HamQSL XML band conditions
        HFConditions cond;
        QXmlStreamReader xml(data);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == "band") {
                QString name = xml.attributes().value("name").toString();
                QString time = xml.attributes().value("time").toString();
                QString val  = xml.readElementText().trimmed();
                int score = (val == "Good") ? 2 : (val == "Fair") ? 1 : 0;
                if (time == "day")   cond.day[name]   = score;
                if (time == "night") cond.night[name] = score;
            }
        }
        cond.valid = !cond.day.isEmpty();
        emit conditionsReady(cond);
        return;
    }

    // Default: JSON station list — merge to preserve local edits/additions
    mergeJson(data);
    emit loaded();
}

void BBSDirectory::parseJson(const QByteArray &data)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (doc.isNull()) {
        qDebug() << "BBSDirectory: JSON parse error:" << err.errorString();
        return;
    }

    m_stations.clear();
    QJsonArray stationArr = doc.object().value("stations").toArray();
    for (const QJsonValue &sv : stationArr) {
        QJsonObject sObj = sv.toObject();
        BBSStation st;
        st.callsign  = sObj.value("callsign").toString();
        st.sysop     = sObj.value("sysop").toString();
        st.network   = sObj.value("network").toString();
        st.location  = sObj.value("location").toString();
        st.grid      = sObj.value("grid").toString();
        st.power     = sObj.value("power").toString();
        st.antenna   = sObj.value("antenna").toString();
        st.notes     = sObj.value("notes").toString();
        st.localOnly = sObj.value("localOnly").toBool(false);

        QJsonArray bandsArr = sObj.value("bands").toArray();
        for (const QJsonValue &bv : bandsArr) {
            QJsonObject bObj = bv.toObject();
            BBSBandEntry be;
            be.band      = bObj.value("band").toString();
            be.frequency = (quint64)bObj.value("frequency").toDouble();
            be.mode      = bObj.value("mode").toString();
            be.hours     = bObj.value("hours").toString();

            QJsonArray bwArr = bObj.value("bandwidth").toArray();
            for (const QJsonValue &bwv : bwArr)
                be.bandwidth.append(bwv.toInt());

            st.bands.append(be);
        }
        m_stations.append(st);
    }
    // parseJson is only used for migration — caller writes to DB separately
}

void BBSDirectory::mergeJson(const QByteArray &data)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (doc.isNull()) {
        qDebug() << "BBSDirectory: JSON parse error:" << err.errorString();
        return;
    }

    QJsonArray stationArr = doc.object().value("stations").toArray();

    // Build set of callsigns in the online JSON
    QSet<QString> onlineCallsigns;
    for (const QJsonValue &sv : stationArr)
        onlineCallsigns.insert(sv.toObject().value("callsign").toString().toUpper());

    // Remove non-local stations that are no longer in the online list
    if (m_db.isOpen()) {
        QSqlQuery sel(m_db);
        sel.exec("SELECT callsign, local_only FROM stations");
        while (sel.next()) {
            QString cs = sel.value(0).toString();
            bool lo    = sel.value(1).toInt() != 0;
            if (!lo && !onlineCallsigns.contains(cs.toUpper())) {
                QSqlQuery del(m_db);
                del.prepare("DELETE FROM stations WHERE callsign = ?");
                del.addBindValue(cs);
                del.exec();
            }
        }
    }

    // Add or update online stations (local-only stations are never touched)
    for (const QJsonValue &sv : stationArr) {
        QJsonObject sObj = sv.toObject();
        QString callsign = sObj.value("callsign").toString();

        // Skip if user has a local-only version — their edits win
        if (m_db.isOpen()) {
            QSqlQuery chk(m_db);
            chk.prepare("SELECT local_only FROM stations WHERE callsign = ? COLLATE NOCASE");
            chk.addBindValue(callsign);
            chk.exec();
            if (chk.next() && chk.value(0).toInt() != 0)
                continue;
        }

        BBSStation st;
        st.callsign  = callsign;
        st.sysop     = sObj.value("sysop").toString();
        st.network   = sObj.value("network").toString();
        st.location  = sObj.value("location").toString();
        st.grid      = sObj.value("grid").toString();
        st.power     = sObj.value("power").toString();
        st.antenna   = sObj.value("antenna").toString();
        st.notes     = sObj.value("notes").toString();
        st.localOnly = false;

        QJsonArray bandsArr = sObj.value("bands").toArray();
        for (const QJsonValue &bv : bandsArr) {
            QJsonObject bObj = bv.toObject();
            BBSBandEntry be;
            be.band      = bObj.value("band").toString();
            be.frequency = (quint64)bObj.value("frequency").toDouble();
            be.mode      = bObj.value("mode").toString();
            be.hours     = bObj.value("hours").toString();
            QJsonArray bwArr = bObj.value("bandwidth").toArray();
            for (const QJsonValue &bwv : bwArr)
                be.bandwidth.append(bwv.toInt());
            st.bands.append(be);
        }
        writeStationToDb(st);
    }
    reloadFromDb();
}

void BBSDirectory::addOrUpdateStation(const BBSStation &station)
{
    writeStationToDb(station);
    reloadFromDb();
}

void BBSDirectory::removeStation(const QString &callsign)
{
    if (!m_db.isOpen()) return;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM stations WHERE callsign = ? COLLATE NOCASE");
    q.addBindValue(callsign);
    q.exec();
    reloadFromDb();
}

bool BBSDirectory::hasStation(const QString &callsign) const
{
    if (!m_db.isOpen()) {
        for (const BBSStation &st : m_stations)
            if (st.callsign.toUpper() == callsign.toUpper())
                return true;
        return false;
    }
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM stations WHERE callsign = ? COLLATE NOCASE");
    q.addBindValue(callsign);
    q.exec();
    return q.next() && q.value(0).toInt() > 0;
}

// hourFilter: current UTC hour (0-23), -1 = skip
// hours field: "00-12" or "12-00" (wraps midnight when start > end)
bool BBSDirectory::hourMatch(const QString &hours, int utcHour) const
{
    if (utcHour < 0) return true;
    QStringList parts = hours.split('-');
    if (parts.size() != 2) return true;
    int start = parts[0].toInt();
    int end   = parts[1].toInt();
    if (start == end) return true; // 00-00 = 24h
    if (start < end)
        return utcHour >= start && utcHour < end;
    else
        return utcHour >= start || utcHour < end;
}

QList<BBSStation> BBSDirectory::filtered(const QString &bandFilter,
                                          int bandwidthFilter,
                                          int hourFilter) const
{
    QList<BBSStation> result;
    for (const BBSStation &st : m_stations) {
        BBSStation filt = st;
        filt.bands.clear();
        for (const BBSBandEntry &be : st.bands) {
            if (!bandFilter.isEmpty() && be.band != bandFilter)
                continue;
            if (bandwidthFilter > 0 && !be.bandwidth.contains(bandwidthFilter))
                continue;
            if (!hourMatch(be.hours, hourFilter))
                continue;
            filt.bands.append(be);
        }
        if (!filt.bands.isEmpty())
            result.append(filt);
    }
    return result;
}

// Convert frequency (Hz) to standard amateur band name
QString BBSDirectory::freqToBand(quint64 freq)
{
    if (freq >= 1800000   && freq <= 2000000)   return "160m";
    if (freq >= 3500000   && freq <= 4000000)   return "80m";
    if (freq >= 5330500   && freq <= 5403500)   return "60m";
    if (freq >= 7000000   && freq <= 7300000)   return "40m";
    if (freq >= 10100000  && freq <= 10150000)  return "30m";
    if (freq >= 14000000  && freq <= 14350000)  return "20m";
    if (freq >= 18068000  && freq <= 18168000)  return "17m";
    if (freq >= 21000000  && freq <= 21450000)  return "15m";
    if (freq >= 24890000  && freq <= 24990000)  return "12m";
    if (freq >= 28000000  && freq <= 29700000)  return "10m";
    if (freq >= 50000000  && freq <= 54000000)  return "6m";
    if (freq >= 144000000 && freq <= 148000000) return "2m";
    return "";
}

bool BBSDirectory::gridToLatLon(const QString &grid, double &lat, double &lon)
{
    QString g = grid.trimmed().toUpper();
    if (g.length() < 4) return false;
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return false;
    if (!g[2].isDigit() || !g[3].isDigit()) return false;

    lon = (g[0].unicode() - 'A') * 20.0 - 180.0;
    lat = (g[1].unicode() - 'A') * 10.0 - 90.0;
    lon += g[2].digitValue() * 2.0;
    lat += g[3].digitValue() * 1.0;

    if (g.length() >= 6) {
        QChar sl = g[4], sr = g[5];
        if (sl >= 'A' && sl <= 'X' && sr >= 'A' && sr <= 'X') {
            lon += (sl.unicode() - 'A') * (2.0 / 24.0) + (1.0 / 24.0);
            lat += (sr.unicode() - 'A') * (1.0 / 24.0) + (0.5 / 24.0);
        } else {
            lon += 1.0; lat += 0.5;
        }
    } else {
        lon += 1.0; lat += 0.5;
    }
    return true;
}

double BBSDirectory::haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat/2)*sin(dLat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dLon/2)*sin(dLon/2);
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

QString BBSDirectory::userGrid()
{
    QString path = QDir::homePath() + "/.config/liaisonos/user.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    // Prefer gridSquare (GPS-updated), fall back to grid (configured)
    QString g = obj.value("gridSquare").toString().trimmed();
    if (g.isEmpty()) g = obj.value("grid").toString().trimmed();
    return g;
}

// Map band name to HamQSL band group
static QString bandGroup(const QString &band)
{
    if (band == "160m" || band == "80m" || band == "40m") return "80m-40m";
    if (band == "30m"  || band == "20m")                  return "30m-20m";
    if (band == "17m"  || band == "15m")                  return "17m-15m";
    if (band == "12m"  || band == "10m")                  return "12m-10m";
    return "30m-20m";
}

// Rough day/night at path midpoint: day if local solar noon ±6h
// Simplified: use longitude midpoint → local noon UTC offset
static bool isDaylight(double midLon, int utcHour)
{
    // Solar noon UTC = 12 - longitude/15
    double noonUtc = 12.0 - midLon / 15.0;
    double diff = fabs(utcHour - noonUtc);
    if (diff > 12) diff = 24 - diff;
    return diff < 7.0;  // within 7h of solar noon = daylight
}

// Distance factor: how well this band covers this distance (0.3-1.0)
static double distanceFactor(const QString &band, double km)
{
    // Each band has an optimal range; score degrades outside it
    struct { const char *b; double lo, hi; } ranges[] = {
        {"160m",   0,  500},
        {"80m",    0, 1000},
        {"40m",  300, 2500},
        {"30m",  500, 4000},
        {"20m", 1000, 6000},
        {"17m", 2000, 8000},
        {"15m", 2000, 9000},
        {"12m", 3000,10000},
        {"10m", 3000,12000},
        {nullptr, 0, 0}
    };
    double lo = 500, hi = 3000;
    for (int i = 0; ranges[i].b; ++i) {
        if (band == ranges[i].b) { lo = ranges[i].lo; hi = ranges[i].hi; break; }
    }
    if (km >= lo && km <= hi) return 1.0;
    if (km < lo) {
        // Too close — near-vertical incidence (NVIS) bands still work short range
        double ratio = km / (lo > 0 ? lo : 1);
        return 0.4 + 0.6 * ratio;
    }
    // Too far
    double over = (km - hi) / hi;
    return qMax(0.2, 1.0 - over * 0.6);
}

int BBSDirectory::connectionProbability(const QString &band, double distKm,
                                        int utcHour, const HFConditions &cond)
{
    // Base score from HamQSL conditions
    int base = 55; // default Fair if no data
    if (cond.valid) {
        QString grp = bandGroup(band);
        // Determine day/night at rough midpoint (use 0 lon as fallback)
        bool day = (utcHour >= 11 && utcHour <= 23);
        int score = day ? cond.day.value(grp, 1) : cond.night.value(grp, 1);
        base = (score == 2) ? 82 : (score == 1) ? 55 : 22;
    }

    double df = distanceFactor(band, distKm);
    int prob = qRound(base * df);
    return qBound(5, prob, 97);
}
