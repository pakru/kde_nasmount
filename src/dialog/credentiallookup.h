/*
 * credentiallookup — SMB credential autofill: the request/reply protocol, the
 * policy deciding whether a returned credential may be offered, and the
 * controller that runs one lookup in a short-lived child process.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A child process rather than a thread because KPasswdServerClient blocks in
 * a nested event loop with no deadline and no cancellation, and may put a
 * wallet prompt on screen: a process can be abandoned, a thread stuck inside
 * a KDE call cannot. Nothing here is privileged or mutating, so no lock is
 * needed; the protocol and policy halves are pure so they are testable
 * without a wallet or a NAS (autofill plan §§3-4).
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QTimer;

namespace Dialog::CredentialLookup
{

/** Carried in every message, so a malformed one fails as a version mismatch. */
constexpr int ProtocolVersion = 1;

/** Ceiling on either message, enforced before parsing: neither side may
 *  buffer or parse an unbounded amount from the other. Far above anything
 *  real, since a credential field is capped at 4096 bytes. */
constexpr qsizetype MaxMessageBytes = 64 * 1024;

/** Total wall-clock budget for one lookup, child startup included. */
constexpr int DeadlineMs = 30000;

/** The private flag selecting this executable's lookup mode. Matched as a
 *  whole argument by both sides, and absent from --help: it is an internal
 *  transport, not a "print my saved password" command. */
QString internalModeFlag();

/** What the parent asks the child to look up. No password travels in argv,
 *  the environment or a URL — only in the child's reply on its pipe. */
struct Request {
    QUrl target;              ///< smb://host/share, from SmbUrl::authLookupTarget()
    QString username;         ///< the *explicit* URL identity only, empty if none
    qulonglong windowId = 0;  ///< native parent-window handle, 0 when there is none
    qulonglong userTime = 0;  ///< X11 user time, 0 when unknown
};

/** What the child answers. `Miss` and `Error` are the same thing to the
 *  caller and differ only so a diagnostic can say which happened. */
struct Reply {
    enum class Outcome {
        Candidate,
        Miss,
        Error,
    };

    Outcome outcome = Outcome::Miss;
    QUrl resultUrl;     ///< the URL of the entry the password service matched
    QString username;   ///< possibly domain-qualified, as KDE stores it
    QString domain;     ///< AuthInfo::realmValue, if the service set one
    QString password;
    QString message;    ///< diagnostic only; never shown to the user
};

/** An eligible credential, split into the three fields the form has. */
struct Candidate {
    QString username;
    QString domain;
    QString password;
};

/** Serialises `request`; empty if it would exceed MaxMessageBytes. */
QByteArray encodeRequest(const Request &request);

/** Strictly parses a request: exact version, known keys only, correct types,
 *  size ceiling, and a target that is `smb://host/share` with exactly one
 *  path component. Anything else is a refusal, not a best-effort read. */
bool decodeRequest(const QByteArray &raw, Request *request, QString *error);

/** Serialises `reply`; empty if oversized. */
QByteArray encodeReply(const Reply &reply);

/** The counterpart of decodeRequest(): same strictness, same ceiling. */
bool decodeReply(const QByteArray &raw, Reply *reply, QString *error);

/**
 * The acceptance policy (plan §3.3), in one pure function.
 *
 * Rejects — meaning "leave the form alone" — unless the reply is a candidate
 * whose URL does not contradict the request, whose fields pass the limits a
 * typed credential passes, whose username is non-empty after `DOMAIN\user`
 * splitting, and which is the same account the smb:// URL named, if it named
 * one. An empty password is accepted; an empty username is not turned into
 * guest selection, and `rejection` never contains credential contents.
 */
bool acceptCandidate(const Reply &reply, const QUrl &requestedTarget,
                     const QString &requestedUsername, Candidate *candidate,
                     QString *rejection);

/** How one lookup's child is spoken to. Abstract so the controller can be
 *  tested against a fake that never answers, answers twice, or answers
 *  garbage. */
class Transport : public QObject
{
    Q_OBJECT

public:
    explicit Transport(QObject *parent = nullptr);
    ~Transport() override;

    /** Sends the request and eventually emits exactly one signal below.
     *  Called at most once. */
    virtual void send(const QByteArray &request) = 0;

    /** Gives up without blocking: this runs on the GUI thread, from window
     *  close and from the deadline. Any waiting for the child to die happens
     *  after it returns, and neither signal may be emitted once it is called. */
    virtual void abandon() = 0;

Q_SIGNALS:
    void replyReceived(const QByteArray &reply);
    void failed(const QString &reason);
};

/** The real transport: one invocation of this executable in its private
 *  lookup mode, spoken to over pipes. */
class ProcessTransport : public Transport
{
    Q_OBJECT

public:
    explicit ProcessTransport(QObject *parent = nullptr);
    ~ProcessTransport() override;

    void send(const QByteArray &request) override;
    void abandon() override;

private:
    class Private;
    Private *d;
};

/**
 * Runs at most one lookup per window and delivers a result exactly once.
 *
 * Every delivery is generation-checked, so a completion already queued when
 * the user edited a field, submitted, or closed the window can no longer be
 * applied.
 */
class Controller : public QObject
{
    Q_OBJECT

public:
    explicit Controller(QObject *parent = nullptr);
    ~Controller() override;

    /** Replaces how children are made; tests inject a fake transport. */
    void setTransportFactory(std::function<Transport *(QObject *)> factory);

    /** Shortens the deadline so its expiry is testable in milliseconds. The
     *  product never calls it. */
    void setDeadlineMs(int milliseconds);

    /** Starts the single lookup. Does nothing for an invalid target or a
     *  second call: one attempt per window, and no retry. */
    void start(const Request &request);

    /** Invalidates the in-flight lookup and stops the child. Safe to call
     *  repeatedly and when nothing is running. */
    void cancel();

    bool isRunning() const;

Q_SIGNALS:
    /** The one delivery of an eligible candidate. The password is a transient
     *  argument, never a property of this object (plan §4.2). */
    void candidateReady(const QString &username, const QString &domain, const QString &password);

    /** Everything else. Not an error to show the user: finding nothing is the
     *  normal case and manual entry was available throughout. */
    void missed(const QString &reason);

private:
    void finishWith(const QString &reason);

    std::function<Transport *(QObject *)> m_factory;
    Transport *m_transport = nullptr;
    QTimer *m_deadline = nullptr;
    Request m_request;
    int m_deadlineMs = DeadlineMs;
    quint64 m_generation = 0;
    bool m_started = false;
    bool m_delivered = false;
};

} // namespace Dialog::CredentialLookup
