/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "credentiallookup.h"
#include "smburl.h"
#include "unitspec.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QTimer>

namespace Dialog::CredentialLookup
{

namespace
{

// Key names are spelled once. The schema is closed — decode rejects any key
// not in these lists — so a typo here would fail the round-trip test rather
// than silently drop a field.
const QLatin1String KeyProtocol("protocol");
const QLatin1String KeyTarget("target");
const QLatin1String KeyUsername("username");
const QLatin1String KeyWindowId("windowId");
const QLatin1String KeyUserTime("userTime");
const QLatin1String KeyOutcome("outcome");
const QLatin1String KeyResultUrl("resultUrl");
const QLatin1String KeyDomain("domain");
const QLatin1String KeyPassword("password");
const QLatin1String KeyMessage("message");

const QLatin1String OutcomeCandidate("candidate");
const QLatin1String OutcomeMiss("miss");
const QLatin1String OutcomeError("error");

/** JSON numbers are doubles, so an integer field is only trustworthy below
 *  2^53. Window handles and X11 timestamps are 32-bit, so this ceiling is
 *  never reached in practice and exists to make a hostile value fail here
 *  rather than silently round. */
constexpr double MaxExactInteger = 9007199254740992.0; // 2^53

bool takeString(const QJsonObject &object, QLatin1String key, QString *out, QString *error)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined() || value.isNull()) {
        out->clear();
        return true;
    }
    if (!value.isString()) {
        *error = QStringLiteral("field '%1' is not a string").arg(QString(key));
        return false;
    }
    *out = value.toString();
    return true;
}

bool takeUnsigned(const QJsonObject &object, QLatin1String key, qulonglong *out, QString *error)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined() || value.isNull()) {
        *out = 0;
        return true;
    }
    if (!value.isDouble()) {
        *error = QStringLiteral("field '%1' is not a number").arg(QString(key));
        return false;
    }
    const double raw = value.toDouble();
    if (raw < 0.0 || raw >= MaxExactInteger || raw != static_cast<double>(static_cast<qulonglong>(raw))) {
        *error = QStringLiteral("field '%1' is not a whole number in range").arg(QString(key));
        return false;
    }
    *out = static_cast<qulonglong>(raw);
    return true;
}

/** Parses one message's outer envelope: size ceiling, valid JSON object,
 *  matching protocol version, and no key outside `allowed`. */
bool openEnvelope(const QByteArray &raw, const QList<QLatin1String> &allowed,
                  QJsonObject *object, QString *error)
{
    if (raw.isEmpty()) {
        *error = QStringLiteral("empty message");
        return false;
    }
    if (raw.size() > MaxMessageBytes) {
        *error = QStringLiteral("message exceeds %1 bytes").arg(MaxMessageBytes);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("not a JSON object: %1").arg(parseError.errorString());
        return false;
    }
    *object = document.object();
    const QJsonValue version = object->value(KeyProtocol);
    if (!version.isDouble() || version.toInt(-1) != ProtocolVersion) {
        *error = QStringLiteral("unsupported protocol version");
        return false;
    }
    for (auto it = object->constBegin(); it != object->constEnd(); ++it) {
        bool known = false;
        for (const QLatin1String &key : allowed) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            *error = QStringLiteral("unknown field '%1'").arg(it.key());
            return false;
        }
    }
    return true;
}

QByteArray serialise(const QJsonObject &object)
{
    const QByteArray encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (encoded.size() > MaxMessageBytes) {
        return QByteArray();
    }
    return encoded;
}

/** The field checks a typed credential passes, applied to an imported one.
 *  Byte-counted, not character-counted: the limit the helper enforces is on
 *  the credentials file it writes. */
bool fieldAcceptable(const QString &value)
{
    return !UnitSpec::hasControlChars(value)
        && value.toUtf8().size() <= UnitSpec::MaxCredentialFieldBytes;
}

/** The share component of a lookup target, decoded. */
QString shareOf(const QUrl &url)
{
    const QStringList parts = url.path(QUrl::FullyDecoded)
                                  .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.size() == 1 ? parts.first() : QString();
}

} // namespace

QByteArray encodeRequest(const Request &request)
{
    QJsonObject object;
    object.insert(KeyProtocol, ProtocolVersion);
    // FullyEncoded, not the default PrettyDecoded: a share named "Media
    // Library" would otherwise be serialised with a literal space, which the
    // strict parse on the other side rejects outright. The two sides of this
    // pipe must agree on one spelling, and the encoded one is the only one
    // that survives a strict re-parse.
    object.insert(KeyTarget, request.target.toString(QUrl::FullyEncoded));
    object.insert(KeyUsername, request.username);
    object.insert(KeyWindowId, static_cast<double>(request.windowId));
    object.insert(KeyUserTime, static_cast<double>(request.userTime));
    return serialise(object);
}

bool decodeRequest(const QByteArray &raw, Request *request, QString *error)
{
    QJsonObject object;
    if (!openEnvelope(raw, {KeyProtocol, KeyTarget, KeyUsername, KeyWindowId, KeyUserTime},
                      &object, error)) {
        return false;
    }

    QString target;
    if (!takeString(object, KeyTarget, &target, error)
        || !takeString(object, KeyUsername, &request->username, error)
        || !takeUnsigned(object, KeyWindowId, &request->windowId, error)
        || !takeUnsigned(object, KeyUserTime, &request->userTime, error)) {
        return false;
    }

    // The child re-derives the target's shape rather than trusting the parent
    // to have sent a sane one: it is a separate process reading a pipe, and
    // "the parent would never do that" is not a property this side can check.
    const QUrl url(target, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("smb") || url.host().isEmpty()
        || url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty() || url.port() != -1
        || shareOf(url).isEmpty()) {
        *error = QStringLiteral("target is not an smb://host/share URL");
        return false;
    }
    if (!fieldAcceptable(request->username)) {
        *error = QStringLiteral("requested username is not a valid credential field");
        return false;
    }
    request->target = url;
    return true;
}

QByteArray encodeReply(const Reply &reply)
{
    QJsonObject object;
    object.insert(KeyProtocol, ProtocolVersion);
    switch (reply.outcome) {
    case Reply::Outcome::Candidate:
        object.insert(KeyOutcome, OutcomeCandidate);
        break;
    case Reply::Outcome::Miss:
        object.insert(KeyOutcome, OutcomeMiss);
        break;
    case Reply::Outcome::Error:
        object.insert(KeyOutcome, OutcomeError);
        break;
    }
    object.insert(KeyResultUrl, reply.resultUrl.toString(QUrl::FullyEncoded));
    object.insert(KeyUsername, reply.username);
    object.insert(KeyDomain, reply.domain);
    object.insert(KeyPassword, reply.password);
    object.insert(KeyMessage, reply.message);
    return serialise(object);
}

bool decodeReply(const QByteArray &raw, Reply *reply, QString *error)
{
    QJsonObject object;
    if (!openEnvelope(raw, {KeyProtocol, KeyOutcome, KeyResultUrl, KeyUsername, KeyDomain,
                            KeyPassword, KeyMessage},
                      &object, error)) {
        return false;
    }

    QString outcome;
    QString resultUrl;
    if (!takeString(object, KeyOutcome, &outcome, error)
        || !takeString(object, KeyResultUrl, &resultUrl, error)
        || !takeString(object, KeyUsername, &reply->username, error)
        || !takeString(object, KeyDomain, &reply->domain, error)
        || !takeString(object, KeyPassword, &reply->password, error)
        || !takeString(object, KeyMessage, &reply->message, error)) {
        return false;
    }

    if (outcome == OutcomeCandidate) {
        reply->outcome = Reply::Outcome::Candidate;
    } else if (outcome == OutcomeMiss) {
        reply->outcome = Reply::Outcome::Miss;
    } else if (outcome == OutcomeError) {
        reply->outcome = Reply::Outcome::Error;
    } else {
        *error = QStringLiteral("unknown outcome");
        return false;
    }
    reply->resultUrl = QUrl(resultUrl, QUrl::StrictMode);
    return true;
}

bool acceptCandidate(const Reply &reply, const QUrl &requestedTarget,
                     const QString &requestedUsername, Candidate *candidate,
                     QString *rejection)
{
    if (reply.outcome != Reply::Outcome::Candidate) {
        *rejection = QStringLiteral("no stored credential for this share");
        return false;
    }

    // Identity sanity: the answer must not *contradict* the question — which
    // is a weaker test than "must repeat the question", and deliberately so.
    //
    // Measured against the real password service (plan §9.4): it answers a
    // share-level question from whichever stored entry matched, and returns
    // that entry's URL rather than the one asked about. A host-level entry
    // for `smb://10.0.0.10/` answers for every share on that server and comes
    // back with no share component at all, while a path-level entry for
    // `smb://nas.local/DATA` comes back with one. Requiring the share
    // components to be equal therefore threw away every credential saved at
    // host level — which is what Dolphin writes by default.
    //
    // So: the scheme and host must match, because a credential for another
    // server is never an answer about this one. The share must match only
    // when the reply actually names one; an answer that names no share is
    // less specific than the question, not a different answer to it. An
    // absent or unparseable URL carries no identity at all and so cannot
    // contradict anything either — it is a reply to the single request this
    // process just made, not an unsolicited one.
    if (reply.resultUrl.isValid() && !reply.resultUrl.isEmpty()) {
        const QString answeredShare = shareOf(reply.resultUrl);
        if (reply.resultUrl.scheme().compare(requestedTarget.scheme(), Qt::CaseInsensitive) != 0
            || reply.resultUrl.host().compare(requestedTarget.host(), Qt::CaseInsensitive) != 0
            || (!answeredShare.isEmpty() && answeredShare != shareOf(requestedTarget))) {
            *rejection = QStringLiteral("the reply describes a different location");
            return false;
        }
    }

    if (!fieldAcceptable(reply.username) || !fieldAcceptable(reply.domain)
        || !fieldAcceptable(reply.password)) {
        // Never says which field or why beyond this: the value itself is a
        // credential, and a length or a character class is still information
        // about it.
        *rejection = QStringLiteral("the stored credential is not usable here");
        return false;
    }

    const SmbUrl::Identity found = SmbUrl::splitDomainUser(reply.username);
    if (found.username.isEmpty()) {
        // Includes the empty-username case, which must not become guest
        // selection: that is the user's choice to make by clearing the field.
        *rejection = QStringLiteral("no usable username in the stored credential");
        return false;
    }
    if (!found.domain.isEmpty() && !reply.domain.isEmpty()
        && found.domain.compare(reply.domain, Qt::CaseInsensitive) != 0) {
        *rejection = QStringLiteral("the stored credential names two different domains");
        return false;
    }

    if (!requestedUsername.isEmpty()) {
        const SmbUrl::Identity asked = SmbUrl::splitDomainUser(requestedUsername);
        // Case-insensitively, because SMB account and domain names are:
        // refusing "Pavel" for a URL that said "pavel" would reject the same
        // account, while accepting "bob" for "alice" would pair one identity
        // with another's secret. Only the second is a real risk.
        if (found.username.compare(asked.username, Qt::CaseInsensitive) != 0) {
            *rejection = QStringLiteral("the stored credential is for a different account");
            return false;
        }
        if (!asked.domain.isEmpty() && !found.domain.isEmpty()
            && asked.domain.compare(found.domain, Qt::CaseInsensitive) != 0) {
            *rejection = QStringLiteral("the stored credential is for a different domain");
            return false;
        }
    }

    candidate->username = found.username;
    // An explicit realm is used only for the contradiction check above, never
    // imported as the domain: for SMB, kio-extras carries the domain inside
    // the username and leaves realmValue alone, so a non-empty realm here is
    // some other protocol's concept and not something to put in a CIFS
    // `domain=` option.
    candidate->domain = found.domain;
    candidate->password = reply.password;
    return true;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

Transport::Transport(QObject *parent)
    : QObject(parent)
{
}

Transport::~Transport() = default;

/** The private mode this executable re-invokes itself in. Matched exactly, as
 *  a whole argument, by both sides. */
static const QLatin1String InternalModeFlag("--internal-credential-lookup");

QString internalModeFlag()
{
    return InternalModeFlag;
}

class ProcessTransport::Private
{
public:
    QProcess *process = nullptr;
    QByteArray buffer;
    bool abandoned = false;
    bool settled = false;
};

ProcessTransport::ProcessTransport(QObject *parent)
    : Transport(parent)
    , d(new Private)
{
}

ProcessTransport::~ProcessTransport()
{
    // The QProcess is deliberately not a child of this object: abandon()
    // hands it to a self-owned cleanup path that outlives the transport, so
    // that destroying the transport can never block on a dying child.
    if (d->process && d->process->parent() == this) {
        abandon();
    }
    delete d;
}

void ProcessTransport::send(const QByteArray &request)
{
    d->process = new QProcess(this);
    // Separate channels, and the child's diagnostics go nowhere: its stderr
    // is not a protocol, and an unread pipe would eventually block a child we
    // are no longer reading from.
    d->process->setProcessChannelMode(QProcess::SeparateChannels);
    d->process->setStandardErrorFile(QProcess::nullDevice());

    connect(d->process, &QProcess::readyReadStandardOutput, this, [this]() {
        if (d->settled) {
            return;
        }
        d->buffer.append(d->process->readAllStandardOutput());
        if (d->buffer.size() > MaxMessageBytes) {
            // Bound the buffer *before* parsing: a child that streams output
            // must not be able to grow the parent's memory.
            d->settled = true;
            abandon();
            Q_EMIT failed(QStringLiteral("the lookup produced too much output"));
        }
    });
    connect(d->process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (d->settled) {
            return;
        }
        d->settled = true;
        const QString reason = d->process->errorString();
        abandon();
        Q_EMIT failed(reason);
    });
    connect(d->process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (d->settled) {
            return;
        }
        d->settled = true;
        d->buffer.append(d->process->readAllStandardOutput());
        if (d->buffer.size() > MaxMessageBytes) {
            Q_EMIT failed(QStringLiteral("the lookup produced too much output"));
            return;
        }
        if (d->buffer.isEmpty()) {
            Q_EMIT failed(QStringLiteral("the lookup exited without answering"));
            return;
        }
        Q_EMIT replyReceived(d->buffer);
    });

    // The absolute path of *this* executable, never a name resolved through
    // PATH and never a shell: the child is this same program in its private
    // mode, and nothing about which program runs may depend on the
    // environment a service menu happened to inherit.
    d->process->start(QCoreApplication::applicationFilePath(), {InternalModeFlag},
                      QIODevice::ReadWrite);
    d->process->write(request);
    d->process->closeWriteChannel();
}

void ProcessTransport::abandon()
{
    d->settled = true;
    if (d->abandoned || !d->process) {
        return;
    }
    d->abandoned = true;

    QProcess *process = d->process;
    d->process = nullptr;
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }

    // Reparent to nothing and let the process own its own end: this function
    // runs on the GUI thread, from window close and from the deadline, and
    // must not wait for anything. Terminate first, escalate to kill after a
    // short grace period, and delete only once it has actually exited —
    // destroying a running QProcess is what would block.
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    process->terminate();
    QTimer::singleShot(2000, process, [process]() {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

Controller::Controller(QObject *parent)
    : QObject(parent)
    , m_factory([](QObject *owner) { return new ProcessTransport(owner); })
{
}

Controller::~Controller()
{
    cancel();
}

void Controller::setTransportFactory(std::function<Transport *(QObject *)> factory)
{
    m_factory = std::move(factory);
}

void Controller::setDeadlineMs(int milliseconds)
{
    m_deadlineMs = milliseconds;
}

bool Controller::isRunning() const
{
    return m_transport != nullptr;
}

void Controller::start(const Request &request)
{
    // One attempt per window (plan §4.1): no retry button, and no second
    // lookup after the user has seen the form.
    if (m_started || !request.target.isValid()) {
        return;
    }
    const QByteArray encoded = encodeRequest(request);
    if (encoded.isEmpty()) {
        return;
    }

    m_started = true;
    m_request = request;
    const quint64 generation = ++m_generation;

    m_transport = m_factory(this);
    connect(m_transport, &Transport::replyReceived, this, [this, generation](const QByteArray &raw) {
        // The generation check is the whole point: this slot can already be
        // queued on the event loop when the user edits a field or closes the
        // window, and cancel() must make that queued delivery inert rather
        // than merely stopping future ones.
        if (generation != m_generation || m_delivered) {
            return;
        }
        Reply reply;
        QString error;
        if (!decodeReply(raw, &reply, &error)) {
            finishWith(error);
            return;
        }
        Candidate candidate;
        QString rejection;
        if (!acceptCandidate(reply, m_request.target, m_request.username, &candidate, &rejection)) {
            finishWith(rejection);
            return;
        }
        m_delivered = true;
        const QString username = candidate.username;
        const QString domain = candidate.domain;
        const QString password = candidate.password;
        // Stop the child and drop our copies before handing the tuple on, so
        // nothing retains it here (plan §4.2). Qt gives no zeroisation
        // guarantee; this is about not keeping it, not about erasing it.
        candidate = Candidate();
        reply = Reply();
        cancel();
        Q_EMIT candidateReady(username, domain, password);
    });
    connect(m_transport, &Transport::failed, this, [this, generation](const QString &reason) {
        if (generation != m_generation || m_delivered) {
            return;
        }
        finishWith(reason);
    });

    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    m_deadline->setInterval(m_deadlineMs);
    connect(m_deadline, &QTimer::timeout, this, [this, generation]() {
        if (generation != m_generation || m_delivered) {
            return;
        }
        // Covers a wallet prompt the user never answers as well as a child
        // that hangs: both are "the window waited long enough", and neither is
        // an error the user is told about.
        finishWith(QStringLiteral("the credential lookup took too long"));
    });
    m_deadline->start();

    m_transport->send(encoded);
}

void Controller::finishWith(const QString &reason)
{
    cancel();
    Q_EMIT missed(reason);
}

void Controller::cancel()
{
    // Invalidate first: everything below can re-enter the event loop, and a
    // queued delivery must already be inert by the time it does.
    ++m_generation;
    if (m_deadline) {
        m_deadline->stop();
        m_deadline->deleteLater();
        m_deadline = nullptr;
    }
    if (m_transport) {
        Transport *transport = m_transport;
        m_transport = nullptr;
        transport->disconnect(this);
        transport->abandon();
        transport->deleteLater();
    }
}

} // namespace Dialog::CredentialLookup
