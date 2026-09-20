/*
 * credentiallookup — the service menu's SMB credential autofill: the bounded
 * request/reply protocol, the policy that decides whether a returned
 * credential may be offered, and the controller that runs one lookup in a
 * short-lived child process (autofill plan §§3-4).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why a child process at all (plan §4.1): the only public way to ask KDE
 * whether it already knows a password for a share is
 * KPasswdServerClient::checkAuthInfo(), which blocks in a nested event loop
 * until kpasswdserver answers — and kpasswdserver may in turn put a wallet
 * unlock prompt on screen and wait for the user. That call offers no
 * deadline and no cancellation, so it cannot be allowed to run anywhere that
 * the window's own shutdown has to wait for: not on the GUI thread, and not
 * on a worker thread either, since a thread stuck inside a KDE call cannot be
 * cancelled without terminating it mid-call. A separate process can simply be
 * abandoned. Killing it does not cancel whatever kpasswdserver has already
 * started — that is KDE's to own — but it does make *our* window's lifetime
 * independent of it.
 *
 * Nothing here is privileged and nothing here mutates: a lookup reads what
 * the user's own session already knows, needs neither Session::UserLock nor
 * Root::RootLock, and the credential it produces is only a suggestion in a
 * form the user still has to submit. The KAuth helper re-validates every
 * field of that submission exactly as it does for typed input.
 *
 * The protocol and policy halves are pure and live here rather than in the
 * worker so they are testable without a wallet, a NAS, or a child process
 * (credentiallookup_test).
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

/** Protocol version carried in every request and reply. A child from a
 *  different version of this binary cannot occur (the parent runs its own
 *  executable path), but the field costs nothing and makes a malformed
 *  message fail as a mismatch rather than as a confusing schema error. */
constexpr int ProtocolVersion = 1;

/**
 * Hard ceiling on either message, in bytes.
 *
 * Deliberately far above anything real (a credential field is capped at
 * UnitSpec::MaxCredentialFieldBytes = 4096) and enforced *before* parsing:
 * the parent must never buffer an unbounded amount of a child's output, and
 * the child must never parse an unbounded request.
 */
constexpr qsizetype MaxMessageBytes = 64 * 1024;

/** Total wall-clock budget for one lookup, child startup included. */
constexpr int DeadlineMs = 30000;

/**
 * The single private command-line flag that selects the lookup mode of this
 * executable (plan §4.1).
 *
 * Spelled once, and matched as a whole argument by both sides: the parent
 * passes exactly this and nothing else, and the child refuses to run unless
 * exactly this and nothing else was passed. It is an internal credential
 * transport, not a user-facing "print my saved password" command, so it is
 * deliberately absent from --help.
 */
QString internalModeFlag();

/** What the parent asks the child to look up. No password is ever passed in
 *  either direction other than in the child's reply on its private pipe —
 *  never in argv, the environment, or a URL (plan §4.1). */
struct Request {
    QUrl target;           ///< smb://host/share, from SmbUrl::authLookupTarget()
    QString username;      ///< the *explicit* URL identity only, empty if none
    qulonglong windowId = 0;  ///< native parent-window handle, 0 when there is none
    qulonglong userTime = 0;  ///< X11 user time for focus-prevention, 0 when unknown
};

/** What the child answers. `Miss` and `Error` are the same thing to the
 *  caller — a lookup that produced nothing — and are distinguished only so a
 *  diagnostic can say which happened. */
struct Reply {
    enum class Outcome {
        Candidate,
        Miss,
        Error,
    };

    Outcome outcome = Outcome::Miss;
    QUrl resultUrl;     ///< the URL the password service echoed back
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

/** Serialises `request` as the child reads it. Returns an empty array if the
 *  result would exceed MaxMessageBytes. */
QByteArray encodeRequest(const Request &request);

/**
 * Strictly parses a request: exact protocol version, known keys only,
 * correct types, size ceiling, and a target that is a valid `smb://host/share`
 * with exactly one path component. Anything else is a refusal, not a
 * best-effort read.
 */
bool decodeRequest(const QByteArray &raw, Request *request, QString *error);

/** Serialises `reply` as the parent reads it. Empty if oversized. */
QByteArray encodeReply(const Reply &reply);

/** The counterpart of decodeRequest(): same strictness, same ceiling. */
bool decodeReply(const QByteArray &raw, Reply *reply, QString *error);

/**
 * The whole of the acceptance policy (plan §3.3), in one pure function.
 *
 * Rejects — meaning "keep the form exactly as it is" — unless all of:
 *
 *  - the reply is a candidate at all;
 *  - the URL it came back with does not contradict the one we asked about
 *    (same scheme, host and share). This is a sanity check on the answer, not
 *    a claim that KDE matched per-share: the password service may legitimately
 *    answer a share-level question from a host-level entry;
 *  - every field passes the same limits a typed credential passes
 *    (UnitSpec::hasControlChars, UnitSpec::MaxCredentialFieldBytes), so an
 *    imported credential cannot get further into the system than one a user
 *    could have entered;
 *  - the username is non-empty after `DOMAIN\user` splitting. An empty
 *    username is *not* turned into guest selection: guest is a choice the user
 *    makes by clearing the field, never one a cache miss makes for them;
 *  - a domain carried inside the username does not contradict an explicit
 *    domain field, which would mean silently picking one of two answers;
 *  - and, when the smb:// URL named a user, the candidate is that same user.
 *    Without this a lookup could answer "you asked about alice, here is bob's
 *    password", and the form would then pair one account's name with another
 *    account's secret.
 *
 * An empty password with a usable username is accepted: username-only
 * autofill is a real benefit, and the empty password is the user's to fill in.
 *
 * `rejection` is for diagnostics and never contains credential contents.
 */
bool acceptCandidate(const Reply &reply, const QUrl &requestedTarget,
                     const QString &requestedUsername, Candidate *candidate,
                     QString *rejection);

/**
 * How one lookup's child process is spoken to. Abstract so the controller can
 * be tested against a fake that answers instantly, never answers, answers
 * twice, or answers garbage — none of which a real child would do on demand.
 */
class Transport : public QObject
{
    Q_OBJECT

public:
    explicit Transport(QObject *parent = nullptr);
    ~Transport() override;

    /** Sends the request and, eventually, emits exactly one of the two
     *  signals below. Called at most once. */
    virtual void send(const QByteArray &request) = 0;

    /**
     * Gives up on this lookup without blocking the caller.
     *
     * Must return immediately: it runs on the GUI thread, from window close
     * and from the deadline timer. Any waiting for the child to die happens
     * asynchronously, after this returns, and neither signal may be emitted
     * once it has been called.
     */
    virtual void abandon() = 0;

Q_SIGNALS:
    void replyReceived(const QByteArray &reply);
    void failed(const QString &reason);
};

/** The real transport: one invocation of this very executable in its private
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
 * Runs at most one lookup for the window's lifetime and hands the result to
 * the host exactly once.
 *
 * Every delivery is generation-checked: cancel() invalidates the in-flight
 * request immediately, so a completion that was already queued on the event
 * loop when the user edited a field, pressed Mount, or closed the window can
 * no longer be applied. That check is what makes "an invalidated result is
 * never applied" true even though the result arrives asynchronously.
 */
class Controller : public QObject
{
    Q_OBJECT

public:
    explicit Controller(QObject *parent = nullptr);
    ~Controller() override;

    /** Replaces how child processes are made. Tests inject a fake transport;
     *  the default builds a ProcessTransport. */
    void setTransportFactory(std::function<Transport *(QObject *)> factory);

    /** Shortens the deadline. Exists so the expiry path can be tested in
     *  milliseconds instead of half a minute; the product never calls it and
     *  DeadlineMs remains the only value shipped. */
    void setDeadlineMs(int milliseconds);

    /**
     * Starts the single lookup. Does nothing if `target` is invalid or a
     * lookup has already been started — there is one attempt per window and
     * no retry (plan §4.1).
     */
    void start(const Request &request);

    /** Invalidates the in-flight lookup and stops the child. Safe to call
     *  repeatedly, and safe to call when nothing is running. */
    void cancel();

    bool isRunning() const;

Q_SIGNALS:
    /** The one and only delivery of an eligible candidate. The password is a
     *  transient argument on purpose: it is never a property of this object
     *  (plan §4.2). */
    void candidateReady(const QString &username, const QString &domain, const QString &password);

    /** Diagnostic completion for everything else. Deliberately not an error
     *  to show the user: a lookup that found nothing is the normal case, and
     *  manual entry was available the whole time. */
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
