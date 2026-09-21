/*
 * Tests for Dialog::CredentialLookup — the SMB credential autofill protocol,
 * its acceptance policy, and the controller's lifetime.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Nothing here contacts a wallet, a password service or a NAS: every
 * credential in this file is synthetic, and the only real child process
 * started is this very test binary, re-invoked in a fake lookup mode so the
 * QProcess transport can be exercised against a process that really starts,
 * really exits, and really answers (or deliberately does not).
 *
 * The two things worth being strict about are the ones a wrong answer would
 * quietly produce rather than fail on: pairing one account's username with
 * another account's password, and applying a result that was invalidated
 * while it was in flight. Both are covered here rather than left to the
 * desktop acceptance run, because neither is visible to someone clicking
 * through the dialog once.
 */

#include "credentiallookup.h"
#include "smburl.h"
#include "unitspec.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include <unistd.h>

static int passed = 0;
static int failed = 0;

static void check(const QString &label, bool condition, const QString &detail = QString())
{
    QTextStream out(stdout);
    out << (condition ? "  PASS  " : "  FAIL  ") << label;
    if (!detail.isEmpty()) {
        out << "   " << detail;
    }
    out << Qt::endl;
    condition ? ++passed : ++failed;
}

using namespace Dialog::CredentialLookup;

namespace
{

/** Selects what the re-invoked child does. Set in the parent's environment
 *  before each ProcessTransport case; read by childMain() below. */
const char *ChildModeVariable = "NASMOUNT_TEST_CHILD_MODE";

QUrl target(const QString &text)
{
    return QUrl(text, QUrl::StrictMode);
}

Reply candidateReply(const QString &username, const QString &password,
                     const QString &domain = QString(),
                     const QString &url = QStringLiteral("smb://nas.example/DATA"))
{
    Reply reply;
    reply.outcome = Reply::Outcome::Candidate;
    reply.resultUrl = target(url);
    reply.username = username;
    reply.password = password;
    reply.domain = domain;
    return reply;
}

/** Runs one acceptance decision and reports both the verdict and, on
 *  acceptance, the three fields that would reach the form. */
void expectAccepted(const QString &label, const Reply &reply, const QString &requested,
                    const QString &expectedUser, const QString &expectedDomain,
                    const QString &expectedPassword)
{
    Candidate candidate;
    QString rejection;
    const bool ok = acceptCandidate(reply, target(QStringLiteral("smb://nas.example/DATA")),
                                    requested, &candidate, &rejection);
    check(label,
          ok && candidate.username == expectedUser && candidate.domain == expectedDomain
              && candidate.password == expectedPassword,
          ok ? QStringLiteral("user=%1 domain=%2 password=%3 chars")
                   .arg(candidate.username, candidate.domain)
                   .arg(candidate.password.size())
             : rejection);
}

void expectRejected(const QString &label, const Reply &reply,
                    const QString &requested = QString(),
                    const QString &requestedTarget = QStringLiteral("smb://nas.example/DATA"))
{
    Candidate candidate;
    QString rejection;
    const bool ok = acceptCandidate(reply, target(requestedTarget), requested, &candidate,
                                    &rejection);
    check(label, !ok && !rejection.isEmpty(),
          ok ? QStringLiteral("accepted as %1").arg(candidate.username) : rejection);
}

// ---------------------------------------------------------------------------
// A transport that answers exactly when the test tells it to.
// ---------------------------------------------------------------------------

class FakeTransport : public Transport
{
public:
    explicit FakeTransport(QObject *parent = nullptr)
        : Transport(parent)
    {
    }

    void send(const QByteArray &request) override
    {
        sent = request;
    }

    void abandon() override
    {
        abandoned = true;
    }

    /** Answers the way a real child would: asynchronously, through the event
     *  loop, which is what makes the generation race reachable at all. */
    void answerLater(const QByteArray &reply)
    {
        QTimer::singleShot(0, this, [this, reply]() { Q_EMIT replyReceived(reply); });
    }

    void failLater(const QString &reason)
    {
        QTimer::singleShot(0, this, [this, reason]() { Q_EMIT failed(reason); });
    }

    QByteArray sent;
    bool abandoned = false;
};

/** Spins the event loop until `done` or `budgetMs` elapses. Never sleeps for
 *  the sake of it: every case here settles on the first pass or two. */
void pump(const std::function<bool()> &done, int budgetMs = 5000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

/** One controller run against a fake transport, reported as a small record. */
struct Run {
    bool gotCandidate = false;
    bool gotMiss = false;
    QString username;
    QString domain;
    QString password;
    QString missReason;
};

void connectRun(Controller *controller, Run *run)
{
    QObject::connect(controller, &Controller::candidateReady, controller,
                     [run](const QString &username, const QString &domain, const QString &password) {
                         run->gotCandidate = true;
                         run->username = username;
                         run->domain = domain;
                         run->password = password;
                     });
    QObject::connect(controller, &Controller::missed, controller, [run](const QString &reason) {
        run->gotMiss = true;
        run->missReason = reason;
    });
}

Request sampleRequest()
{
    Request request;
    request.target = target(QStringLiteral("smb://nas.example/DATA"));
    request.username = QString();
    return request;
}

// ---------------------------------------------------------------------------
// The fake child: this same binary, re-invoked by ProcessTransport.
// ---------------------------------------------------------------------------

int childMain()
{
    const QByteArray mode = qgetenv(ChildModeVariable);

    QFile input;
    if (!input.open(STDIN_FILENO, QIODevice::ReadOnly)) {
        return 1;
    }
    const QByteArray request = input.read(MaxMessageBytes + 1);

    if (mode == "silent") {
        return 0; // exits without answering
    }
    if (mode == "crash") {
        ::_exit(9);
    }

    QFile output;
    if (!output.open(STDOUT_FILENO, QIODevice::WriteOnly)) {
        return 1;
    }
    if (mode == "garbage") {
        output.write("this is not JSON");
    } else if (mode == "flood") {
        // Deliberately past the ceiling, to prove the parent bounds what it
        // buffers instead of growing to fit whatever a child produces.
        output.write(QByteArray(MaxMessageBytes + 4096, 'x'));
    } else if (mode == "echo-target") {
        // Proves the request really crossed the pipe: answer with a candidate
        // whose URL is whatever the parent asked about.
        Request decoded;
        QString error;
        Reply reply;
        if (decodeRequest(request, &decoded, &error)) {
            reply.outcome = Reply::Outcome::Candidate;
            reply.resultUrl = decoded.target;
            reply.username = QStringLiteral("nasuser");
            reply.password = QStringLiteral("synthetic-secret");
        } else {
            reply.outcome = Reply::Outcome::Error;
            reply.message = error;
        }
        output.write(encodeReply(reply));
    } else {
        Reply reply;
        reply.outcome = Reply::Outcome::Miss;
        output.write(encodeReply(reply));
    }
    output.flush();
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == internalModeFlag()) {
        return childMain();
    }

    QTextStream out(stdout);
    out << "credentiallookup_test" << Qt::endl;

    // --- protocol: round trips ---------------------------------------------
    {
        // Built the way the product builds it, from a UNC whose share name
        // contains a space: the round trip has to survive QUrl's encoding of
        // it, or a "Media Library" share would reach the password service
        // under a different name than Dolphin used.
        Request request;
        request.target = Dialog::SmbUrl::authLookupTarget(
            QStringLiteral("//nas.example/Media Library/Films"));
        request.username = QStringLiteral("WORKGROUP\\pavel");
        request.windowId = 123456;
        request.userTime = 42;
        Request decoded;
        QString error;
        const bool ok = decodeRequest(encodeRequest(request), &decoded, &error);
        check(QStringLiteral("request round trip"),
              ok && decoded.target == request.target && decoded.username == request.username
                  && decoded.windowId == request.windowId && decoded.userTime == request.userTime
                  && decoded.target.path(QUrl::FullyDecoded)
                      == QStringLiteral("/Media Library"),
              ok ? decoded.target.toString() : error);
    }
    {
        Reply reply = candidateReply(QStringLiteral("pavel"), QStringLiteral("p@ss w0rd"),
                                     QStringLiteral("WORKGROUP"));
        Reply decoded;
        QString error;
        const bool ok = decodeReply(encodeReply(reply), &decoded, &error);
        check(QStringLiteral("reply round trip"),
              ok && decoded.outcome == Reply::Outcome::Candidate
                  && decoded.username == reply.username && decoded.password == reply.password
                  && decoded.domain == reply.domain && decoded.resultUrl == reply.resultUrl,
              error);
    }

    // --- protocol: strictness ----------------------------------------------
    {
        auto rejects = [&out](const QString &label, const QByteArray &raw) {
            Request request;
            QString error;
            const bool ok = decodeRequest(raw, &request, &error);
            check(label, !ok && !error.isEmpty(), ok ? QStringLiteral("accepted") : error);
        };
        rejects(QStringLiteral("request: empty"), QByteArray());
        rejects(QStringLiteral("request: not JSON"), QByteArrayLiteral("{nope"));
        rejects(QStringLiteral("request: not an object"), QByteArrayLiteral("[1,2,3]"));
        rejects(QStringLiteral("request: wrong protocol version"),
                QByteArrayLiteral(R"({"protocol":2,"target":"smb://h/s"})"));
        rejects(QStringLiteral("request: unknown field"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s","extra":1})"));
        rejects(QStringLiteral("request: target is not a string"),
                QByteArrayLiteral(R"({"protocol":1,"target":7})"));
        rejects(QStringLiteral("request: windowId is not a number"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s","windowId":"7"})"));
        rejects(QStringLiteral("request: negative windowId"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s","windowId":-1})"));
        rejects(QStringLiteral("request: fractional windowId"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s","windowId":1.5})"));
        rejects(QStringLiteral("request: server-only target"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/"})"));
        rejects(QStringLiteral("request: target keeps a subdirectory"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s/sub"})"));
        rejects(QStringLiteral("request: non-smb target"),
                QByteArrayLiteral(R"({"protocol":1,"target":"https://h/s"})"));
        rejects(QStringLiteral("request: target carries user info"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://u@h/s"})"));
        rejects(QStringLiteral("request: target carries a port"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h:445/s"})"));
        rejects(QStringLiteral("request: target carries a query"),
                QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s?x=1"})"));
        rejects(QStringLiteral("request: username with a control character"),
                QByteArrayLiteral("{\"protocol\":1,\"target\":\"smb://h/s\",\"username\":\"a\\nb\"}"));

        // The ceiling is enforced before parsing, so an oversized message is
        // refused even when it is perfectly well-formed JSON.
        QByteArray oversized = QByteArrayLiteral(R"({"protocol":1,"target":"smb://h/s","username":")");
        oversized.append(QByteArray(MaxMessageBytes, 'u'));
        oversized.append(QByteArrayLiteral(R"("})"));
        rejects(QStringLiteral("request: over the size ceiling"), oversized);
    }
    {
        Reply reply;
        QString error;
        check(QStringLiteral("reply: unknown outcome"),
              !decodeReply(QByteArrayLiteral(R"({"protocol":1,"outcome":"maybe"})"), &reply, &error),
              error);
        check(QStringLiteral("reply: truncated JSON"),
              !decodeReply(QByteArrayLiteral(R"({"protocol":1,"outcome":"candi)"), &reply, &error),
              error);
        check(QStringLiteral("reply: unknown field"),
              !decodeReply(QByteArrayLiteral(R"({"protocol":1,"outcome":"miss","x":1})"), &reply,
                           &error),
              error);
    }
    {
        // An encoder that would exceed the ceiling produces nothing rather
        // than a message the other side must then refuse.
        Reply huge;
        huge.outcome = Reply::Outcome::Candidate;
        huge.password = QString(MaxMessageBytes, QLatin1Char('x'));
        check(QStringLiteral("reply: oversized encode refuses"), encodeReply(huge).isEmpty());
    }

    // --- policy: what may be applied ---------------------------------------
    expectAccepted(QStringLiteral("plain username and password"),
                   candidateReply(QStringLiteral("pavel"), QStringLiteral("secret")), QString(),
                   QStringLiteral("pavel"), QString(), QStringLiteral("secret"));
    expectAccepted(QStringLiteral("DOMAIN\\user splits"),
                   candidateReply(QStringLiteral("WORKGROUP\\pavel"), QStringLiteral("secret")),
                   QString(), QStringLiteral("pavel"), QStringLiteral("WORKGROUP"),
                   QStringLiteral("secret"));
    expectAccepted(QStringLiteral("DOMAIN/user splits"),
                   candidateReply(QStringLiteral("WORKGROUP/pavel"), QStringLiteral("secret")),
                   QString(), QStringLiteral("pavel"), QStringLiteral("WORKGROUP"),
                   QStringLiteral("secret"));
    expectAccepted(QStringLiteral("a UPN stays one username"),
                   candidateReply(QStringLiteral("pavel@example.com"), QStringLiteral("secret")),
                   QString(), QStringLiteral("pavel@example.com"), QString(),
                   QStringLiteral("secret"));
    expectAccepted(QStringLiteral("empty password is still usable"),
                   candidateReply(QStringLiteral("pavel"), QString()), QString(),
                   QStringLiteral("pavel"), QString(), QString());
    expectAccepted(QStringLiteral("explicit URL identity matches"),
                   candidateReply(QStringLiteral("pavel"), QStringLiteral("secret")),
                   QStringLiteral("pavel"), QStringLiteral("pavel"), QString(),
                   QStringLiteral("secret"));
    expectAccepted(QStringLiteral("explicit URL identity matches case-insensitively"),
                   candidateReply(QStringLiteral("Pavel"), QStringLiteral("secret")),
                   QStringLiteral("pavel"), QStringLiteral("Pavel"), QString(),
                   QStringLiteral("secret"));
    expectAccepted(QStringLiteral("qualified candidate matches a bare URL identity"),
                   candidateReply(QStringLiteral("WORKGROUP\\pavel"), QStringLiteral("secret")),
                   QStringLiteral("pavel"), QStringLiteral("pavel"), QStringLiteral("WORKGROUP"),
                   QStringLiteral("secret"));
    expectAccepted(QStringLiteral("a realm agreeing with the qualified domain is fine"),
                   candidateReply(QStringLiteral("WORKGROUP\\pavel"), QStringLiteral("secret"),
                                  QStringLiteral("workgroup")),
                   QString(), QStringLiteral("pavel"), QStringLiteral("WORKGROUP"),
                   QStringLiteral("secret"));

    // The shape the real password service actually returns (plan §9.4): a
    // host-level entry answers a share-level question and comes back with no
    // share component. Rejecting this threw away every credential Dolphin
    // saves by default, which is the bug that made autofill appear not to
    // work at all on a real NAS.
    expectAccepted(QStringLiteral("a host-level entry answers for the share"),
                   candidateReply(QStringLiteral("pa_kru"), QStringLiteral("secret"), QString(),
                                  QStringLiteral("smb://nas.example/")),
                   QString(), QStringLiteral("pa_kru"), QString(), QStringLiteral("secret"));
    expectAccepted(QStringLiteral("a path-level entry answers for its own share"),
                   candidateReply(QStringLiteral("pa_kru"), QStringLiteral("secret"), QString(),
                                  QStringLiteral("smb://nas.example/DATA")),
                   QString(), QStringLiteral("pa_kru"), QString(), QStringLiteral("secret"));
    {
        // No identity at all is not a contradiction either: the reply answers
        // the single request this process made.
        Reply anonymous = candidateReply(QStringLiteral("pa_kru"), QStringLiteral("secret"));
        anonymous.resultUrl = QUrl();
        expectAccepted(QStringLiteral("a reply with no URL is not a contradiction"), anonymous,
                       QString(), QStringLiteral("pa_kru"), QString(), QStringLiteral("secret"));
    }
    {
        Reply miss;
        miss.outcome = Reply::Outcome::Miss;
        expectRejected(QStringLiteral("a miss is not a candidate"), miss);
        Reply error;
        error.outcome = Reply::Outcome::Error;
        expectRejected(QStringLiteral("an error is not a candidate"), error);
    }
    expectRejected(QStringLiteral("a reply about another host"),
                   candidateReply(QStringLiteral("pavel"), QStringLiteral("secret"), QString(),
                                  QStringLiteral("smb://other.example/DATA")));
    expectRejected(QStringLiteral("a host-level reply about another host"),
                   candidateReply(QStringLiteral("pavel"), QStringLiteral("secret"), QString(),
                                  QStringLiteral("smb://other.example/")));
    expectRejected(QStringLiteral("a reply about another share"),
                   candidateReply(QStringLiteral("pavel"), QStringLiteral("secret"), QString(),
                                  QStringLiteral("smb://nas.example/BACKUP")));
    expectRejected(QStringLiteral("empty username is not guest selection"),
                   candidateReply(QString(), QStringLiteral("secret")));
    expectRejected(QStringLiteral("a username that is only a separator"),
                   candidateReply(QStringLiteral("WORKGROUP\\"), QStringLiteral("secret")));
    expectRejected(QStringLiteral("a newline in the password"),
                   candidateReply(QStringLiteral("pavel"), QStringLiteral("sec\nret")));
    expectRejected(QStringLiteral("a newline in the username"),
                   candidateReply(QStringLiteral("pa\nvel"), QStringLiteral("secret")));
    expectRejected(QStringLiteral("a password past the credential field limit"),
                   candidateReply(QStringLiteral("pavel"),
                                  QString(UnitSpec::MaxCredentialFieldBytes + 1,
                                          QLatin1Char('x'))));
    expectRejected(QStringLiteral("two different domains in one reply"),
                   candidateReply(QStringLiteral("WORKGROUP\\pavel"), QStringLiteral("secret"),
                                  QStringLiteral("OTHERDOM")));
    expectRejected(QStringLiteral("another account's credential"),
                   candidateReply(QStringLiteral("bob"), QStringLiteral("secret")),
                   QStringLiteral("alice"));
    expectRejected(QStringLiteral("the same name in another domain"),
                   candidateReply(QStringLiteral("OTHERDOM\\alice"), QStringLiteral("secret")),
                   QStringLiteral("WORKGROUP\\alice"));

    // --- controller: delivery and invalidation ------------------------------
    {
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        FakeTransport *transport = nullptr;
        controller.setTransportFactory([&transport](QObject *owner) {
            transport = new FakeTransport(owner);
            return transport;
        });
        controller.start(sampleRequest());
        check(QStringLiteral("controller: the request reached the transport"),
              transport != nullptr && !transport->sent.isEmpty());
        transport->answerLater(encodeReply(candidateReply(QStringLiteral("pavel"),
                                                          QStringLiteral("secret"))));
        pump([&run]() { return run.gotCandidate || run.gotMiss; });
        check(QStringLiteral("controller: delivers an eligible candidate"),
              run.gotCandidate && run.username == QStringLiteral("pavel")
                  && run.password == QStringLiteral("secret"),
              run.missReason);
        check(QStringLiteral("controller: stops the child after delivering"),
              !controller.isRunning());
    }
    {
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        FakeTransport *transport = nullptr;
        controller.setTransportFactory([&transport](QObject *owner) {
            transport = new FakeTransport(owner);
            return transport;
        });
        controller.start(sampleRequest());
        // The queued completion and the cancellation race here exactly as
        // they do when a reply arrives in the same event-loop turn as the
        // user pressing Mount.
        transport->answerLater(encodeReply(candidateReply(QStringLiteral("pavel"),
                                                          QStringLiteral("secret"))));
        const bool abandonedOnCancel = (controller.cancel(), transport->abandoned);
        pump([]() { return false; }, 200);
        check(QStringLiteral("controller: a queued reply after cancel is dropped"),
              !run.gotCandidate && !run.gotMiss);
        check(QStringLiteral("controller: cancel abandons the child"), abandonedOnCancel);
    }
    {
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        FakeTransport *transport = nullptr;
        controller.setTransportFactory([&transport](QObject *owner) {
            transport = new FakeTransport(owner);
            return transport;
        });
        controller.start(sampleRequest());
        FakeTransport *first = transport;
        controller.start(sampleRequest());
        check(QStringLiteral("controller: one lookup per window, no retry"), transport == first);
    }
    {
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        controller.setTransportFactory([](QObject *owner) { return new FakeTransport(owner); });
        controller.setDeadlineMs(50);
        controller.start(sampleRequest()); // nothing ever answers
        pump([&run]() { return run.gotMiss; });
        check(QStringLiteral("controller: the deadline ends a silent lookup"),
              run.gotMiss && !run.gotCandidate, run.missReason);
    }
    {
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        FakeTransport *transport = nullptr;
        controller.setTransportFactory([&transport](QObject *owner) {
            transport = new FakeTransport(owner);
            return transport;
        });
        controller.start(sampleRequest());
        transport->answerLater(QByteArrayLiteral("{not json"));
        pump([&run]() { return run.gotMiss || run.gotCandidate; });
        check(QStringLiteral("controller: malformed output is a miss, not a crash"),
              run.gotMiss && !run.gotCandidate, run.missReason);
    }
    {
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        FakeTransport *transport = nullptr;
        controller.setTransportFactory([&transport](QObject *owner) {
            transport = new FakeTransport(owner);
            return transport;
        });
        controller.start(sampleRequest());
        transport->failLater(QStringLiteral("no password service"));
        pump([&run]() { return run.gotMiss || run.gotCandidate; });
        check(QStringLiteral("controller: an unavailable service is a miss"),
              run.gotMiss && !run.gotCandidate, run.missReason);
    }
    {
        // A candidate for the wrong account must not reach the form even
        // though the transport delivered it successfully.
        Controller controller;
        Run run;
        connectRun(&controller, &run);
        FakeTransport *transport = nullptr;
        controller.setTransportFactory([&transport](QObject *owner) {
            transport = new FakeTransport(owner);
            return transport;
        });
        Request request = sampleRequest();
        request.username = QStringLiteral("alice");
        controller.start(request);
        transport->answerLater(encodeReply(candidateReply(QStringLiteral("bob"),
                                                          QStringLiteral("secret"))));
        pump([&run]() { return run.gotMiss || run.gotCandidate; });
        check(QStringLiteral("controller: refuses another account's credential"),
              run.gotMiss && !run.gotCandidate, run.missReason);
    }

    // --- the real process transport ----------------------------------------
    // These re-invoke this binary, so they exercise QProcess startup, the
    // pipes, and child exit for real.
    {
        struct ChildCase {
            const char *mode;
            QString label;
            bool expectCandidate;
        };
        const ChildCase cases[] = {
            {"echo-target", QStringLiteral("child: a real candidate crosses the pipe"), true},
            {"miss", QStringLiteral("child: a miss is reported as a miss"), false},
            {"silent", QStringLiteral("child: exiting without answering is a miss"), false},
            {"garbage", QStringLiteral("child: unparseable output is a miss"), false},
            {"flood", QStringLiteral("child: oversized output is bounded and refused"), false},
            {"crash", QStringLiteral("child: a crash is a miss"), false},
        };
        for (const ChildCase &testCase : cases) {
            qputenv(ChildModeVariable, testCase.mode);
            Controller controller;
            Run run;
            connectRun(&controller, &run);
            controller.setDeadlineMs(10000);
            controller.start(sampleRequest());
            pump([&run]() { return run.gotCandidate || run.gotMiss; }, 10000);
            const bool ok = testCase.expectCandidate
                ? (run.gotCandidate && run.username == QStringLiteral("nasuser")
                   && run.password == QStringLiteral("synthetic-secret"))
                : (run.gotMiss && !run.gotCandidate);
            check(testCase.label, ok, run.gotCandidate ? run.username : run.missReason);
        }
        qunsetenv(ChildModeVariable);
    }

    out << (failed == 0 ? "all passed" : "FAILURES") << ": " << passed << " passed, " << failed
        << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
