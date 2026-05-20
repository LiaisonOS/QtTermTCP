#ifndef BBSDIRECTORY_H
#define BBSDIRECTORY_H

#include <QObject>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlDatabase>

// HF band conditions from HamQSL — score 0=Poor, 1=Fair, 2=Good, -1=unknown
struct HFConditions {
    QMap<QString, int> day;    // key: "80m-40m","30m-20m","17m-15m","12m-10m"
    QMap<QString, int> night;
    bool valid = false;
};

struct BBSBandEntry {
    QString band;           // "40m", "80m", etc.
    quint64 frequency;      // Hz
    QString mode;           // "VARA HF"
    QList<int> bandwidth;   // [500, 2300]
    QString hours;          // "00-12" UTC
};

struct BBSStation {
    QString callsign;
    QString sysop;
    QString network;
    QString location;
    QString grid;
    QString power;
    QString antenna;
    QString notes;
    QList<BBSBandEntry> bands;
    bool localOnly = false;  // true = user-added, never overwritten by online refresh
};

class BBSDirectory : public QObject
{
    Q_OBJECT

public:
    explicit BBSDirectory(QObject *parent = nullptr);

    void loadFromFile(const QString &path);
    void fetchFromUrl(const QString &url);

    const QList<BBSStation> &stations() const { return m_stations; }

    // Add or update a station (matched by callsign)
    void addOrUpdateStation(const BBSStation &station);
    void removeStation(const QString &callsign);
    bool hasStation(const QString &callsign) const;

    // Filtered view
    // bandwidthFilter: 500/2300/2750 (0 = no filter)
    // hourFilter: current UTC hour 0-23 (-1 = no filter)
    // bandFilter: e.g. "40m" (empty = no filter)
    QList<BBSStation> filtered(const QString &bandFilter,
                               int bandwidthFilter,
                               int hourFilter) const;

    QString localFilePath() const;  // returns path to .db file

    // Convert frequency (Hz) to amateur band name
    static QString freqToBand(quint64 freq);

    // Maidenhead grid to lat/lon (returns false if grid invalid)
    static bool gridToLatLon(const QString &grid, double &lat, double &lon);

    // Haversine distance in km between two lat/lon points
    static double haversineKm(double lat1, double lon1, double lat2, double lon2);

    // Read user grid from ~/.config/emcomm-tools/user.json
    static QString userGrid();

    // Fetch HF band conditions from HamQSL (non-blocking)
    void fetchHFConditions();

    // Connection probability 0-100 given band, distance km, utc hour, conditions
    static int connectionProbability(const QString &band, double distKm,
                                     int utcHour, const HFConditions &cond);

signals:
    void loaded();
    void fetchError(const QString &error);
    void conditionsReady(HFConditions cond);

private slots:
    void onNetworkReply(QNetworkReply *reply);

private:
    void initDb();
    void reloadFromDb();
    void writeStationToDb(const BBSStation &station);
    void parseJson(const QByteArray &data);
    void mergeJson(const QByteArray &data);
    bool hourMatch(const QString &hours, int utcHour) const;
    QString jsonFilePath() const;

    QList<BBSStation> m_stations;
    QNetworkAccessManager *m_nam;
    QSqlDatabase m_db;
};

// Global instance — shared between QtTermTCP and TabDialog
extern BBSDirectory *g_bbsDirectory;

#endif // BBSDIRECTORY_H
