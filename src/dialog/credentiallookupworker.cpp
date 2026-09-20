/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "credentiallookupworker.h"
#include "credentiallookup.h"

#include <KIO/AuthInfo>
#include <KPasswdServerClient>

#include <QCoreApplication>
#include <QFile>

#include <sys/stat.h>
#include <unistd.h>

namespace Dialog::CredentialLookupWorker
{

namespace
{

/** Exit codes. The parent ignores them — a reply on stdout is the only thing
 *  it reads — but they keep a hand-run invocation honest. */
constexpr int ExitOk = 0;
constexpr int ExitRefused = 2;

/** A pipe or a socket, and nothing else. A terminal means someone ran this by
 *  hand; a regular file means output would be left on disk. Both are refused
 *  before the request is even read, so neither can end up holding a
 *  credential. */
bool isPipeLike(int fd)
{
    struct stat info;
    if (::fstat(fd, &info) != 0) {
        return false;
    }
    return S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode);
}

/** Reads at most one byte more than the ceiling, so "too big" is detectable
 *  without ever holding an unbounded amount of it. */
bool readRequest(QByteArray *raw)
{
    QFile input;
    if (!input.open(STDIN_FILENO, QIODevice::ReadOnly)) {
        return false;
    }
    *raw = input.read(CredentialLookup::MaxMessageBytes + 1);
    return raw->size() <= CredentialLookup::MaxMessageBytes;
}

void writeReply(const CredentialLookup::Reply &reply)
{
    const QByteArray encoded = CredentialLookup::encodeReply(reply);
    if (encoded.isEmpty()) {
        return;
    }
    QFile output;
    if (!output.open(STDOUT_FILENO, QIODevice::WriteOnly)) {
        return;
    }
    output.write(encoded);
    output.flush();
}

/** A reply that carries no credential, for every path that is not a hit. */
CredentialLookup::Reply barrenReply(CredentialLookup::Reply::Outcome outcome, const QString &message)
{
    CredentialLookup::Reply reply;
    reply.outcome = outcome;
    reply.message = message;
    return reply;
}

} // namespace

bool isInternalInvocation(int argc, char **argv)
{
    // Exactly one argument, matched whole. Not a prefix, not one flag among
    // others: a mode that hands back a credential must be impossible to enter
    // by accident while passing something else.
    return argc == 2 && argv[1] != nullptr
        && QString::fromLocal8Bit(argv[1]) == CredentialLookup::internalModeFlag();
}

int run(int argc, char **argv)
{
    // QCoreApplication, never QApplication: this process has no window, no
    // display connection and no QML engine. It does need an event loop type,
    // because the password-service client answers through D-Bus.
    QCoreApplication app(argc, argv);

    if (!isPipeLike(STDIN_FILENO) || !isPipeLike(STDOUT_FILENO)) {
        return ExitRefused;
    }

    QByteArray raw;
    if (!readRequest(&raw)) {
        writeReply(barrenReply(CredentialLookup::Reply::Outcome::Error,
                               QStringLiteral("unreadable request")));
        return ExitRefused;
    }

    CredentialLookup::Request request;
    QString error;
    if (!CredentialLookup::decodeRequest(raw, &request, &error)) {
        writeReply(barrenReply(CredentialLookup::Reply::Outcome::Error, error));
        return ExitRefused;
    }

    // The exact shape kio-extras' SMB authenticator uses (plan §3.2): the
    // already-narrowed share URL with no user-info component, the username
    // supplied separately, an empty password, and verifyPath set. Nothing
    // here invents a realm, and nothing asks for anything to be *stored* —
    // keepPassword stays false, because this is a read of what the session
    // already knows.
    KIO::AuthInfo info;
    info.url = request.target;
    info.username = request.username;
    info.verifyPath = true;

    KPasswdServerClient client;
    // checkAuthInfo() only ever reports what is already known; queryAuthInfo()
    // — the call that would put a password dialog on screen and ask — is
    // deliberately never used. Autofill must never become a second
    // authentication prompt (plan §1). The service may still ask the user to
    // unlock a wallet to answer, which is KDE's own behaviour and the reason
    // this runs where it can be abandoned.
    const bool answered = client.checkAuthInfo(&info, request.windowId, request.userTime);

    CredentialLookup::Reply reply;
    if (!answered || !info.isModified()) {
        // Both halves matter: a successful call that did not modify the info
        // means "nothing known", which is the ordinary case for a share the
        // user has never opened.
        reply = barrenReply(CredentialLookup::Reply::Outcome::Miss,
                            QStringLiteral("no stored credential"));
    } else {
        reply.outcome = CredentialLookup::Reply::Outcome::Candidate;
        reply.resultUrl = info.url;
        reply.username = info.username;
        reply.domain = info.realmValue;
        reply.password = info.password;
    }

    writeReply(reply);
    // Drop the copies this process holds. Qt offers no zeroisation guarantee
    // for QString, so this is "do not keep it", not "erase it from memory" —
    // the real bound on exposure is that this process exits immediately.
    info.password.clear();
    reply.password.clear();
    return ExitOk;
}

} // namespace Dialog::CredentialLookupWorker
