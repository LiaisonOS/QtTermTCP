/*
 * BulletinSender.cpp
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#include "BulletinSender.h"

#include <QCoreApplication>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QHostAddress>
#include <QCryptographicHash>

#include <cstdio>

// fprintf-based logger that bypasses Qt's logging routing entirely. Qt's
// qCritical/qWarning silently redirect to journald when stderr isn't a tty
// (the case the moment a pipe is involved), which makes headless-mode
// debugging impossible. Raw fprintf+fflush always writes to fd 2.
#define BSLOG(...) do { \
        fprintf(stderr, "[BulletinSender] " __VA_ARGS__); \
        fputc('\n', stderr); \
        fflush(stderr); \
    } while (0)


// LinBPQ confirmation line, verified against a real BPQ-6.0.25 session log:
//   "Message: 10841 Bid: 10841_VA2OPS Size: 23"
// We pin Bid:\s*(\S+) specifically (was previously \bBID[:\s]+\S+ which also
// matched things like "old BID was X" in welcome banners).
static const QRegularExpression BID_RE(
    "\\bBid:\\s*(\\S+)",
    QRegularExpression::CaseInsensitiveOption);

// Message number on the same line — captured into bulletin metadata for
// operator-friendly receipts.
static const QRegularExpression MSG_NUM_RE(
    "\\bMessage:\\s*(\\d+)",
    QRegularExpression::CaseInsensitiveOption);

// Catch ERROR / Cannot / Invalid / Rejected / Unable lines so we exit-3
// instead of timing out.
static const QRegularExpression BBS_ERROR_RE(
    "\\b(ERROR|Invalid|Cannot|Rejected|Unable)\\b",
    QRegularExpression::CaseInsensitiveOption);

// State-specific prompts, all verified against BPQ-6.0.25's actual phrasing.
// Using state-specific matches instead of a generic "(>|:)$" because the body
// prompt "Enter Message Text (end with /ex or ctrl/z)" ends with ')' — the
// generic regex would have wedged there.

// BPQ Telnet login prompts. Different BPQ builds phrase these slightly
// differently (Login:, Callsign:, User:), so we accept all the common forms.
// Anchor-free intentionally: BPQ doesn't always terminate the prompt with
// a newline (it sits waiting for input after "Callsign: "), so end-of-line
// anchors would never fire. The state machine clears the buffer after each
// transition so the same keyword can't re-trigger.
static const QRegularExpression TELNET_USER_PROMPT_RE(
    "(?:user|callsign|login)\\s*:",
    QRegularExpression::CaseInsensitiveOption);
static const QRegularExpression TELNET_PASS_PROMPT_RE(
    "password\\s*:",
    QRegularExpression::CaseInsensitiveOption);
// After successful Telnet login, BPQ shows its top menu and then waits for
// a command like "bbs" or "B" to enter the BBS. Verified phrasing from the
// captured BPQ-6.0.25 log: "Enter ? for list of commands".
static const QRegularExpression TELNET_MENU_RE(
    "Enter\\s+\\?\\s+for\\s+list\\s+of\\s+commands",
    QRegularExpression::CaseInsensitiveOption);

// Initial connected-to-BBS prompt: "de <CALL>>"
static const QRegularExpression BBS_INITIAL_PROMPT_RE(
    "\\bde\\s+\\S+\\s*>",
    QRegularExpression::CaseInsensitiveOption);

static const QRegularExpression BBS_TITLE_PROMPT_RE(
    "Enter\\s+Title",
    QRegularExpression::CaseInsensitiveOption);

static const QRegularExpression BBS_BODY_PROMPT_RE(
    "Enter\\s+Message\\s+Text",
    QRegularExpression::CaseInsensitiveOption);

// VARA events on the control socket.
static const QRegularExpression VARA_CONNECTED_RE(
    "^CONNECTED\\b", QRegularExpression::CaseInsensitiveOption);
static const QRegularExpression VARA_DISCONNECTED_RE(
    "^DISCONNECTED\\b", QRegularExpression::CaseInsensitiveOption);


int BulletinSender::runFromArgs(const QStringList &args)
{
    BSLOG("runFromArgs invoked with %d arg(s)", args.size());

    QString bulletinId, connectUrl, missionId;
    QString callsign, telnetUser, telnetPwd;
    QString modeFlag;
    int timeoutMin = 15;

    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        auto next = [&](const QString &flag, QString *out) {
            if (a == flag && i + 1 < args.size()) {
                *out = args[++i];
                return true;
            }
            return false;
        };
        if (next("--send-bulletin", &bulletinId)) continue;
        if (next("--connect",       &connectUrl)) continue;
        if (next("--mission-id",    &missionId))  continue;
        if (next("--callsign",      &callsign))   continue;
        if (next("--user",          &telnetUser)) continue;
        if (next("--pwd",           &telnetPwd))  continue;
        if (next("--mode",          &modeFlag))   continue;
        if (a == "--timeout" && i + 1 < args.size()) {
            timeoutMin = args[++i].toInt();
            if (timeoutMin <= 0) timeoutMin = 15;
            continue;
        }
    }

    if (bulletinId.isEmpty()) {
        BSLOG("error: --send-bulletin <id> is required.");
        return 5;
    }
    if (connectUrl.isEmpty()) {
        BSLOG("error: --connect <url> is required.");
        return 5;
    }
    if (!modeFlag.isEmpty() && modeFlag != "vara_hf" && modeFlag != "telnet") {
        BSLOG("error: Phase 2 supports --mode vara_hf and --mode telnet only "
              "(got %s).", qUtf8Printable(modeFlag));
        return 5;
    }
    BSLOG("args ok: id=%s url=%s mode=%s timeout=%d",
          qUtf8Printable(bulletinId), qUtf8Printable(connectUrl),
          qUtf8Printable(modeFlag.isEmpty() ? "<from-url>" : modeFlag),
          timeoutMin);

    // Mode-specific credential checks. Telnet to a LinBPQ node requires
    // username + password (BPQ prompts for both); RF (VARA) requires the
    // operator's callsign as MYCALL on the modem control channel.
    bool isTelnet = connectUrl.toLower().startsWith("telnet:")
                    || modeFlag == "telnet";
    if (isTelnet) {
        if (telnetUser.isEmpty()) {
            BSLOG("error: --user <username> is required for --mode telnet.");
            return 5;
        }
        if (telnetPwd.isEmpty()) {
            BSLOG("error: --pwd <password> is required for --mode telnet.");
            return 5;
        }
    } else {
        if (callsign.isEmpty()) {
            BSLOG("error: --callsign <CALL> is required for --mode vara_hf.");
            return 5;
        }
    }

    BulletinSender sender;
    sender.m_telnetUser = telnetUser;
    sender.m_telnetPwd  = telnetPwd;
    return sender.run(bulletinId, connectUrl, timeoutMin, missionId,
                      callsign.toUpper());
}


BulletinSender::BulletinSender(QObject *parent)
    : QObject(parent)
{
    m_udp = new QUdpSocket(this);
}


int BulletinSender::run(const QString &bulletinId,
                        const QString &connectUrl,
                        int timeoutMin,
                        const QString &missionId,
                        const QString &myCall)
{
    m_bulletinId = bulletinId;
    m_missionId = missionId;
    m_myCall = myCall;

    QString err;
    if (!loadBulletin(bulletinId, &err)) {
        BSLOG("error: %s", qUtf8Printable(err));
        return 5;
    }
    BSLOG("bulletin loaded: title=\"%s\" to=%s at_bbs=%s type=%s body=%d chars",
          qUtf8Printable(m_bulletin.title), qUtf8Printable(m_bulletin.to),
          qUtf8Printable(m_bulletin.atBbs), qUtf8Printable(m_bulletin.type),
          m_bulletin.body.size());

    parseConnectUrl(connectUrl);
    if (m_targetBbs.isEmpty()) {
        BSLOG("error: could not extract target BBS callsign from --connect %s",
              qUtf8Printable(connectUrl));
        return 5;
    }
    BSLOG("parsed url: mode=%s host=%s port=%d targetBbs=%s",
          (m_mode == ModeTelnet ? "telnet" : "vara_hf"),
          qUtf8Printable(m_mode == ModeTelnet ? m_telnetHost : m_varaHost),
          (m_mode == ModeTelnet ? m_telnetPort : m_varaCmdPort),
          qUtf8Printable(m_targetBbs));

    m_overallTimeout = new QTimer(this);
    m_overallTimeout->setSingleShot(true);
    connect(m_overallTimeout, &QTimer::timeout,
            this, &BulletinSender::onTimeout);
    m_overallTimeout->start(timeoutMin * 60 * 1000);

    if (m_mode == ModeTelnet) {
        m_telnet = new QTcpSocket(this);
        connect(m_telnet, &QTcpSocket::connected,
                this, &BulletinSender::onTelnetConnected);
        connect(m_telnet, &QTcpSocket::readyRead,
                this, &BulletinSender::onTelnetReadyRead);
        connect(m_telnet, &QTcpSocket::errorOccurred,
                this, [this](QAbstractSocket::SocketError) {
                    if (m_state < Done)
                        finishFailed(2, "Telnet: " + m_telnet->errorString());
                });
        connect(m_telnet, &QTcpSocket::disconnected,
                this, [this]() {
                    if (m_state == Disconnecting) {
                        emitEvent("disconnected");
                        finishOk(m_capturedBid);
                    } else if (m_state < Done) {
                        finishFailed(2, "Telnet closed before send completed");
                    }
                });
        connectTelnet();
    } else {
        m_control = new QTcpSocket(this);
        m_data    = new QTcpSocket(this);
        connect(m_control, &QTcpSocket::connected,
                this, &BulletinSender::onVaraConnected);
        connect(m_control, &QTcpSocket::readyRead,
                this, &BulletinSender::onVaraReadyRead);
        connect(m_control, &QTcpSocket::errorOccurred,
                this, [this](QAbstractSocket::SocketError) {
                    if (m_state < Done)
                        finishFailed(2, "VARA control: " + m_control->errorString());
                });
        connect(m_data, &QTcpSocket::readyRead,
                this, &BulletinSender::onDataReadyRead);
        connect(m_data, &QTcpSocket::errorOccurred,
                this, [this](QAbstractSocket::SocketError) {
                    if (m_state < Done)
                        finishFailed(2, "VARA data: " + m_data->errorString());
                });
        connectVara();
    }
    return QCoreApplication::exec();
}


void BulletinSender::parseConnectUrl(const QString &url)
{
    QUrl u(url);
    QString scheme = u.scheme().toLower();
    QUrlQuery q(u);

    if (scheme == "telnet") {
        // telnet://host:port/<BBS_LABEL>
        // host:port is the LinBPQ telnet listener; user / password come from
        // the --user / --pwd flags because they're credentials, not URL data.
        m_mode = ModeTelnet;
        m_telnetHost = u.host();
        m_telnetPort = (u.port() > 0) ? u.port() : 8772;
        // m_targetBbs is purely cosmetic for receipts here — Telnet doesn't
        // dial a callsign the way VARA does. Defaults to the host name.
        m_targetBbs = u.path();
        while (m_targetBbs.startsWith('/')) m_targetBbs.remove(0, 1);
        if (m_targetBbs.isEmpty()) m_targetBbs = m_telnetHost;
        m_targetBbs = m_targetBbs.toUpper();
        return;
    }

    // varahf:///VE2BBS?freq=7102&bw=2300
    m_mode = ModeVaraHf;
    m_targetBbs = u.path();
    while (m_targetBbs.startsWith('/')) m_targetBbs.remove(0, 1);
    m_targetBbs = m_targetBbs.toUpper();
    if (q.hasQueryItem("bw")) {
        bool ok = false;
        int bw = q.queryItemValue("bw").toInt(&ok);
        if (ok && bw > 0) m_bandwidth = bw;
    }
    QSettings s;
    QString h = s.value("VARAHostHF", "127.0.0.1").toString();
    int p = s.value("VARAPortHF", 8300).toInt();
    if (!h.isEmpty()) m_varaHost = h;
    if (p > 0)        m_varaCmdPort = p;
}


void BulletinSender::connectTelnet()
{
    m_state = ConnectingTelnet;
    BSLOG("connectTelnet -> %s:%d",
          qUtf8Printable(m_telnetHost), m_telnetPort);
    emitEvent("connecting", m_targetBbs);
    m_telnet->connectToHost(m_telnetHost, m_telnetPort);
}


void BulletinSender::onTelnetConnected()
{
    BSLOG("TCP connected — waiting for user/callsign prompt");
    emitEvent("connected");
    m_state = WaitingTelnetUserPrompt;
}


void BulletinSender::onTelnetReadyRead()
{
    QByteArray chunk = m_telnet->readAll();
    BSLOG("rx %d bytes (state=%d)", chunk.size(), (int)m_state);
    // Show the actual bytes so we can see what BPQ is sending — useful for
    // tuning the login prompt regex without a packet capture.
    QString preview = QString::fromUtf8(chunk).left(200);
    preview.replace('\r', "\\r").replace('\n', "\\n");
    BSLOG("rx preview: %s", qUtf8Printable(preview));
    processBbsBytes(chunk);
}


bool BulletinSender::loadBulletin(const QString &id, QString *errorOut)
{
    QString err;
    if (BulletinStore::load(id, "pending", &m_bulletin, &err)) return true;
    if (errorOut)
        *errorOut = QString("Bulletin '%1' not in pending/: %2").arg(id, err);
    return false;
}


void BulletinSender::emitEvent(const QString &event, const QString &detail,
                               const QString &extraKey, const QString &extraVal)
{
    QJsonObject o;
    o["at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    o["event"] = event;
    if (!m_missionId.isEmpty()) o["mission_id"] = m_missionId;
    if (!detail.isEmpty())      o["detail"]     = detail;
    if (!extraKey.isEmpty())    o[extraKey]     = extraVal;
    QByteArray payload = QJsonDocument(o).toJson(QJsonDocument::Compact);
    m_udp->writeDatagram(payload, QHostAddress::LocalHost, 7456);
}


void BulletinSender::connectVara()
{
    m_state = ConnectingVara;
    emitEvent("connecting", m_targetBbs);
    m_control->connectToHost(m_varaHost, m_varaCmdPort);
}


void BulletinSender::onVaraConnected()
{
    m_state = VaraHandshake;
    writeControl("MYCALL " + m_myCall.toUtf8() + "\r");
    writeControl("BW " + QByteArray::number(m_bandwidth) + "\r");

    // VARA's data port is conventionally cmd_port + 1.
    m_data->connectToHost(m_varaHost, m_varaCmdPort + 1);

    m_state = Dialing;
    writeControl("CONNECT " + m_myCall.toUtf8()
                 + " " + m_targetBbs.toUtf8() + "\r");
}


void BulletinSender::writeControl(const QByteArray &line)
{
    if (m_control && m_control->state() == QAbstractSocket::ConnectedState)
        m_control->write(line);
}


void BulletinSender::writeData(const QByteArray &payload)
{
    if (m_data && m_data->state() == QAbstractSocket::ConnectedState)
        m_data->write(payload);
}


void BulletinSender::onVaraReadyRead()
{
    m_controlBuf.append(m_control->readAll());
    int idx;
    while ((idx = m_controlBuf.indexOf('\r')) >= 0) {
        QString line = QString::fromUtf8(m_controlBuf.left(idx)).trimmed();
        m_controlBuf.remove(0, idx + 1);
        if (!line.isEmpty()) processControlLine(line);
    }
}


void BulletinSender::processControlLine(const QString &line)
{
    if (VARA_CONNECTED_RE.match(line).hasMatch()) {
        m_state = WaitingBbsPrompt;
        emitEvent("connected");
        return;
    }
    if (VARA_DISCONNECTED_RE.match(line).hasMatch()) {
        if (m_state == Disconnecting) {
            emitEvent("disconnected");
            finishOk(m_capturedBid);
        } else if (m_state < Done) {
            finishFailed(2, "VARA reported DISCONNECTED before send completed");
        }
        return;
    }
    // Other VARA events (PENDING, BUFFER, IAMALIVE, …) are noise for our
    // purposes; keep them silent rather than mistaking one for a fault.
}


void BulletinSender::onDataReadyRead()
{
    processBbsBytes(m_data->readAll());
}


// Unified write-to-BBS — VARA goes through the data socket, Telnet through
// its single socket. State-machine logic above this layer doesn't care which.
static void writeBbs(QTcpSocket *s, const QByteArray &payload)
{
    if (s && s->state() == QAbstractSocket::ConnectedState)
        s->write(payload);
}


void BulletinSender::processBbsBytes(const QByteArray &chunk)
{
    m_bbsBuf.append(chunk);
    QString text = QString::fromUtf8(m_bbsBuf);

    QTcpSocket *bbs = (m_mode == ModeTelnet) ? m_telnet : m_data;

    // BID line — fire from WaitingForBid only so welcome-banner mentions of
    // "BID" in history can't trigger a false sent event.
    if (m_state == WaitingForBid) {
        auto bidMatch = BID_RE.match(text);
        if (bidMatch.hasMatch() && m_capturedBid.isEmpty()) {
            m_capturedBid = bidMatch.captured(1);
            auto msgMatch = MSG_NUM_RE.match(text);
            if (msgMatch.hasMatch())
                m_capturedMsgNum = msgMatch.captured(1);
            BSLOG("captured BID=%s msg=%s -> disconnecting",
                  qUtf8Printable(m_capturedBid),
                  qUtf8Printable(m_capturedMsgNum));
            emitEvent("sent", QString(), "ack",
                      "BID:" + m_capturedBid +
                      (m_capturedMsgNum.isEmpty()
                          ? QString()
                          : " (msg " + m_capturedMsgNum + ")"));
            m_state = Disconnecting;
            // Send "B" (bye); the BBS will tear down. For Telnet this also
            // returns us to the LinBPQ menu; the disconnected signal handler
            // takes us to finishOk.
            QByteArray eol = (m_mode == ModeTelnet) ? "\r\n" : "\r";
            writeBbs(bbs, "B" + eol);
            // Telnet abrupt close as a safety net if BPQ doesn't drop us.
            if (m_mode == ModeTelnet) {
                QTimer::singleShot(2000, this, [this]() {
                    if (m_state == Disconnecting && m_telnet)
                        m_telnet->disconnectFromHost();
                });
            }
            return;
        }
        if (BBS_ERROR_RE.match(text).hasMatch()) {
            finishFailed(3, "BBS rejected: " + text.right(200));
            return;
        }
        return;
    }

    if (BBS_ERROR_RE.match(text).hasMatch() &&
        m_state >= SendingHeader && m_state < WaitingForBid) {
        finishFailed(3, "BBS rejected: " + text.right(200));
        return;
    }

    // State-specific prompt detection. Each transition uses the EXACT prompt
    // BPQ emits at that step instead of a generic terminator regex, because
    // the body prompt "Enter Message Text (end with /ex or ctrl/z)" ends in
    // ')' and would have wedged a generic state machine.
    switch (m_state) {
    case WaitingTelnetUserPrompt:
        if (TELNET_USER_PROMPT_RE.match(text).hasMatch()) {
            BSLOG("matched user prompt -> send user '%s'",
                  qUtf8Printable(m_telnetUser));
            m_state = WaitingTelnetPassPrompt;
            writeBbs(bbs, m_telnetUser.toUtf8() + "\r\n");
            m_bbsBuf.clear();
        }
        break;
    case WaitingTelnetPassPrompt:
        if (TELNET_PASS_PROMPT_RE.match(text).hasMatch()) {
            BSLOG("matched password prompt -> send password");
            m_state = WaitingTelnetMenu;
            writeBbs(bbs, m_telnetPwd.toUtf8() + "\r\n");
            m_bbsBuf.clear();
        }
        break;
    case WaitingTelnetMenu:
        if (TELNET_MENU_RE.match(text).hasMatch()) {
            BSLOG("matched node menu -> send 'bbs'");
            m_state = WaitingBbsPrompt;
            writeBbs(bbs, "bbs\r\n");
            m_bbsBuf.clear();
        }
        break;
    case WaitingBbsPrompt:
        if (BBS_INITIAL_PROMPT_RE.match(text).hasMatch()) {
            BSLOG("matched BBS prompt -> send header");
            m_state = SendingHeader;
            QByteArray cmd;
            if (m_bulletin.type == "P")
                cmd = "SP " + m_bulletin.to.toUtf8();
            else
                cmd = "SB " + m_bulletin.to.toUtf8();
            if (!m_bulletin.atBbs.isEmpty() &&
                m_bulletin.atBbs.toUpper() != m_targetBbs.toUpper()) {
                cmd += " @ " + m_bulletin.atBbs.toUtf8();
            }
            // Telnet semantics: CR alone = carriage return (cursor home);
            // CRLF = newline. BPQ over telnet wants CRLF on every line.
            // VARA over RF uses bare CR — the modem layer handles framing.
            cmd += (m_mode == ModeTelnet) ? "\r\n" : "\r";
            writeBbs(bbs, cmd);
            m_bbsBuf.clear();
        }
        break;
    case SendingHeader:
        if (BBS_TITLE_PROMPT_RE.match(text).hasMatch()) {
            BSLOG("matched title prompt -> send title \"%s\"",
                  qUtf8Printable(m_bulletin.title));
            m_state = SendingTitle;
            QByteArray eol = (m_mode == ModeTelnet) ? "\r\n" : "\r";
            writeBbs(bbs, m_bulletin.title.toUtf8() + eol);
            m_bbsBuf.clear();
        }
        break;
    case SendingTitle:
        if (BBS_BODY_PROMPT_RE.match(text).hasMatch()) {
            BSLOG("matched body prompt -> send body (%d chars) + /ex",
                  m_bulletin.body.size());
            m_state = SendingBody;
            QByteArray eol = (m_mode == ModeTelnet) ? "\r\n" : "\r";
            QString body = m_bulletin.body;
            if (!body.endsWith('\n')) body.append('\n');
            QByteArray out = body.toUtf8();
            out.replace("\n", eol);    // body lines get the right newline
            out += "/ex";
            out += eol;                // /ex on its own line, properly terminated
            emitEvent("sending", QString(), "bytes",
                      QString::number(out.size()));
            writeBbs(bbs, out);
            m_state = WaitingForBid;
            BSLOG("now waiting for BID line from BPQ");
            m_bbsBuf.clear();
        }
        break;
    default:
        break;
    }
}


void BulletinSender::onTimeout()
{
    BSLOG("overall timeout fired in state=%d", (int)m_state);
    if (m_state == Done) return;
    if (m_state < Dialing) {
        finishFailed(2, "Timeout before VARA link came up");
    } else if (m_state == WaitingForBid && !m_capturedBid.isEmpty()) {
        // We saw a BID but VARA never reported DISCONNECTED. Treat as ok.
        finishOk(m_capturedBid);
    } else if (m_state == WaitingForBid) {
        finishFailed(4, "Body sent but no BID before timeout");
    } else {
        finishFailed(2, "Timeout mid-session");
    }
}


// Compute MD5 of the TXT body file as it is on disk right NOW. This is the
// content we actually sent (read-then-send semantics) and what gets recorded
// as last_sent_md5 — so the daemon's next pre-fire check can decide "same
// content as last time → silent skip" or "content changed → fire".
static QString currentBodyMd5(const QString &id, const QString &subDir)
{
    QFile body(BulletinStore::rootDir() + "/" + subDir + "/" + id + ".txt");
    if (!body.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Md5);
    h.addData(body.readAll());
    return QString::fromLatin1(h.result().toHex());
}


void BulletinSender::finishOk(const QString &capturedBid)
{
    if (m_state == Done) return;
    m_state = Done;

    // Files DO NOT move out of pending/. Instead, the JSON metadata is
    // updated in place with last_sent_md5 / last_sent_at / last_sent_bid /
    // send_count. The daemon uses last_sent_md5 to silently skip future
    // mission fires until the TXT content actually changes (operator edit
    // or external regenerator script). One-shot bulletins stay quiet
    // forever; recurring ones re-fire automatically on each TXT change.
    m_bulletin.status        = "sent";
    m_bulletin.sentAt        = QDateTime::currentDateTime();
    m_bulletin.bid           = capturedBid;
    m_bulletin.messageNumber = m_capturedMsgNum;
    m_bulletin.lastError.clear();

    m_bulletin.lastSentMd5 = currentBodyMd5(m_bulletin.id, "pending");
    m_bulletin.lastSentBid = capturedBid;
    m_bulletin.lastSentAt  = m_bulletin.sentAt.toString(Qt::ISODate);
    m_bulletin.sendCount   += 1;

    BSLOG("finishOk: last_sent_md5=%s send_count=%d (file stays in pending/)",
          qUtf8Printable(m_bulletin.lastSentMd5),
          m_bulletin.sendCount);

    QString err;
    BulletinStore::save(m_bulletin, &err);
    // BulletinStore::save also rewrites the body file from title + body. We
    // computed the MD5 BEFORE the save above, which is the correct moment —
    // it's the bytes we actually transmitted, not whatever ended up on disk
    // after the metadata round-trip (which is identical in practice but
    // semantically the right anchor point).

    QCoreApplication::exit(0);
}


void BulletinSender::finishFailed(int exitCode, const QString &reason)
{
    if (m_state == Done) return;
    m_state = Failed;
    m_exitCode = exitCode;
    // Surface the reason to stderr too — the UDP feed alone leaves the
    // operator with just an exit code when they run from the shell.
    BSLOG("FAIL exit=%d state=%d reason=%s",
          exitCode, (int)m_state, qUtf8Printable(reason));
    emitEvent("failed", reason, "exit",
              QString::number(exitCode));

    m_bulletin.status    = "failed";
    m_bulletin.lastError = reason;
    m_bulletin.retryCount += 1;
    QString err;
    BulletinStore::save(m_bulletin, &err);

    QCoreApplication::exit(exitCode);
}
