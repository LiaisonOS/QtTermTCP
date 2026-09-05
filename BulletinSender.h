/*
 * BulletinSender.h
 *
 * Headless BBS bulletin sender for QtTermTCP, driven by
 *
 *   qttermtcp --send-bulletin <id>
 *             --mode vara_hf
 *             --connect varahf:///<BBS>?freq=N&bw=B
 *             --timeout <minutes>
 *             [--mission-id <id>]
 *             [--mycall <call>]
 *
 * Phase 2 v1: VARA HF only — other modems (VARA FM, Mercury, HF300, packet)
 * are deliberately deferred so the state machine, BID detection, UDP feed and
 * exit-code wiring can be exercised against a real LinBPQ node with minimal
 * moving parts.
 *
 * Runs inside a normal QCoreApplication event loop with no MainWindow. The
 * sender emits the same UDP progress events on 127.0.0.1:7456 as
 * QtPatWinlink's --auto-mission so li-listen surfaces both uniformly.
 *
 * Exit codes (matches the project_bbs_publish_mission spec):
 *    0  sent, BID captured
 *    2  connect failed (VARA / link down / VARA TCP unreachable)
 *    3  BBS rejected (auth, malformed addr, ERROR pattern)
 *    4  sent but no BID seen before timeout
 *    5  bulletin file missing or invalid CLI args
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#ifndef BULLETINSENDER_H
#define BULLETINSENDER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QByteArray>
#include <QDateTime>

#include "Bulletin.h"


class BulletinSender : public QObject
{
    Q_OBJECT

public:
    static int runFromArgs(const QStringList &args);

private:
    enum Mode {
        ModeVaraHf,
        ModeTelnet,
    };

    enum State {
        Idle,
        ConnectingVara,
        ConnectingTelnet,
        VaraHandshake,
        Dialing,
        WaitingTelnetUserPrompt,
        WaitingTelnetPassPrompt,
        WaitingTelnetMenu,
        WaitingBbsPrompt,
        SendingHeader,
        WaitingTitlePrompt,
        SendingTitle,
        WaitingBodyPrompt,
        SendingBody,
        WaitingForBid,
        Disconnecting,
        Done,
        Failed,
    };

    BulletinSender(QObject *parent = nullptr);

    int run(const QString &bulletinId,
            const QString &connectUrl,
            int timeoutMin,
            const QString &missionId,
            const QString &myCall);

    void parseConnectUrl(const QString &url);
    bool loadBulletin(const QString &id, QString *errorOut);
    void emitEvent(const QString &event, const QString &detail = QString(),
                   const QString &extraKey = QString(),
                   const QString &extraVal = QString());

    void connectVara();
    void onVaraConnected();
    void onVaraReadyRead();
    void onDataReadyRead();
    void connectTelnet();
    void onTelnetConnected();
    void onTelnetReadyRead();
    void onTimeout();

    void writeControl(const QByteArray &line);
    void writeData(const QByteArray &payload);
    void processControlLine(const QString &line);
    void processBbsBytes(const QByteArray &chunk);

    void finishOk(const QString &capturedBid);
    void finishFailed(int exitCode, const QString &reason);

    Mode         m_mode  = ModeVaraHf;
    State        m_state = Idle;
    QTcpSocket  *m_control = nullptr;
    QTcpSocket  *m_data    = nullptr;
    QTcpSocket  *m_telnet  = nullptr;
    QUdpSocket  *m_udp     = nullptr;
    QTimer      *m_overallTimeout = nullptr;

    QString      m_bulletinId;
    QString      m_missionId;
    QString      m_myCall;
    QString      m_targetBbs;
    int          m_bandwidth = 2300;
    QString      m_varaHost = "127.0.0.1";
    int          m_varaCmdPort = 8300;
    QString      m_telnetHost;
    int          m_telnetPort = 8772;
    QString      m_telnetUser;
    QString      m_telnetPwd;

    Bulletin     m_bulletin;
    QByteArray   m_controlBuf;
    QByteArray   m_bbsBuf;
    QString      m_capturedBid;
    QString      m_capturedMsgNum;
    int          m_exitCode = 0;
};

#endif
