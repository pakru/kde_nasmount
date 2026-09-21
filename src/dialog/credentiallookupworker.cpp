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

/** The parent reads only stdout; these keep a hand-run invocation honest. */
constexpr int ExitOk = 0;
constexpr int ExitRefused = 2;

/** A pipe or socket only: a terminal means a hand-run invocation, a regular
 *  file means output left on disk. Refused before the request is read. */
bool isPipeLike(int fd)
{
    struct stat info;
    if (::fstat(fd, &info) != 0) {
        return false;
    }
    return S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode);
}

/** One byte past the ceiling, so "too big" is detectable without holding
 *  an unbounded amount. */
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
    // Matched whole, not as a prefix: a mode that returns a credential must
    // be impossible to enter by accident.
    return argc == 2 && argv[1] != nullptr
        && QString::fromLocal8Bit(argv[1]) == CredentialLookup::internalModeFlag();
}

int run(int argc, char **argv)
{
    // QCoreApplication, never QApplication: no window, no display, no QML —
    // but an event loop is needed, since the client answers over D-Bus.
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

    // The exact shape kio-extras' SMB authenticator uses (plan §3.2): share
    // URL with no user-info, username supplied separately, empty password,
    // verifyPath set. No realm is invented and keepPassword stays false.
    KIO::AuthInfo info;
    info.url = request.target;
    info.username = request.username;
    info.verifyPath = true;

    KPasswdServerClient client;
    // checkAuthInfo() only reports what is known; queryAuthInfo(), which
    // would prompt, is never used — autofill must not become a second
    // password dialog (plan §1). The service may still ask to unlock a
    // wallet, which is why this runs where it can be abandoned.
    const bool answered = client.checkAuthInfo(&info, request.windowId, request.userTime);

    CredentialLookup::Reply reply;
    if (!answered || !info.isModified()) {
        // Both halves matter: a successful call that modified nothing means
        // "nothing known".
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
    // Qt offers no zeroisation guarantee, so this is "do not keep it", not
    // "erase it"; the real bound is that this process exits immediately.
    info.password.clear();
    reply.password.clear();
    return ExitOk;
}

} // namespace Dialog::CredentialLookupWorker
