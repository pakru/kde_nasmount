/*
 * Tests for the credential half of ShareForm.qml — the *real* file, loaded
 * from the source tree, not a C++ model of it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One rule here cannot be checked anywhere else: an imported credential must
 * never be paired with a manually entered one (SMB credential autofill plan
 * §§5, 8.1). Both halves of that rule live in QML — the seal set by
 * TextField.textEdited, and applyCredentialSuggestion()'s refusal to apply
 * anything once it is set — and QML resolves neither at compile time. A
 * duplicate model in C++ would keep passing this test while the form it
 * mirrors drifted, which is precisely the drift this project has already had
 * once between its two front ends.
 *
 * The user's edits are simulated by emitting the field's own textEdited
 * signal rather than by synthesising key events: it is the signal the form
 * actually binds to, and it is reachable without a window, a compositor or a
 * font. Programmatic assignment is used where the *host* would assign, so the
 * test also pins the distinction the whole feature rests on — an assignment
 * is not an edit.
 *
 * No real credential appears here; every value is synthetic, and none of it
 * is written anywhere.
 */

#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTextStream>
#include <QVariant>

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

namespace
{

/**
 * Stands in for Session::MountActions so submit() has something to call.
 *
 * Records the tuple the form hands over, which is the only way to check the
 * end of the chain: that an imported password is submitted as the user saw
 * it, through the same addShare() a typed one goes through.
 */
class RecordingActions : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void addShare(const QString &unc, const QString &rawMountPoint,
                              const QString &username, const QString &domain,
                              const QString &password, const QString &access)
    {
        ++calls;
        lastUnc = unc;
        lastMountPoint = rawMountPoint;
        lastUsername = username;
        lastDomain = domain;
        lastPassword = password;
        lastAccess = access;
    }

    int calls = 0;
    QString lastUnc;
    QString lastMountPoint;
    QString lastUsername;
    QString lastDomain;
    QString lastPassword;
    QString lastAccess;
};

QObject *fieldNamed(QObject *form, const char *name)
{
    return form->findChild<QObject *>(QString::fromLatin1(name));
}

QString textOf(QObject *form, const char *name)
{
    QObject *field = fieldNamed(form, name);
    return field ? field->property("text").toString() : QStringLiteral("<no such field>");
}

/** What the user doing something in a field looks like to the form. */
void simulateEdit(QObject *form, const char *name, const QString &text)
{
    QObject *field = fieldNamed(form, name);
    if (!field) {
        return;
    }
    field->setProperty("text", text);
    QMetaObject::invokeMethod(field, "textEdited");
}

bool applySuggestion(QObject *form, const QString &username, const QString &domain,
                     const QString &password)
{
    QVariant result;
    QMetaObject::invokeMethod(form, "applyCredentialSuggestion", Q_RETURN_ARG(QVariant, result),
                              Q_ARG(QVariant, username), Q_ARG(QVariant, domain),
                              Q_ARG(QVariant, password));
    return result.toBool();
}

void callMethod(QObject *form, const char *name)
{
    QMetaObject::invokeMethod(form, name);
}

} // namespace

int main(int argc, char **argv)
{
    // No window is ever shown: the form is a ColumnLayout, and every rule
    // under test is a property and two functions. Offscreen keeps this
    // runnable in a package-build container, and the Basic style keeps it
    // independent of whichever Plasma style happens to be installed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);

    QTextStream out(stdout);
    out << "shareform_qml_test" << Qt::endl;

    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(NASMOUNT_SHAREFORM_QML)));
    if (component.isError()) {
        out << "  FAIL  ShareForm.qml does not load   " << component.errorString() << Qt::endl;
        return 1;
    }

    RecordingActions actions;

    auto freshForm = [&component, &actions, &out]() -> QObject * {
        QObject *form = component.create();
        if (!form) {
            out << "  FAIL  ShareForm.qml could not be instantiated   " << component.errorString()
                << Qt::endl;
            return nullptr;
        }
        form->setProperty("actions", QVariant::fromValue(static_cast<QObject *>(&actions)));
        form->setProperty("fixedUnc", QStringLiteral("//nas.example/DATA"));
        return form;
    };

    // --- the ordinary autofill path ----------------------------------------
    {
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        // The host fills the form in first, exactly as the service-menu
        // window does from backend.suggestedUser. This must not look like an
        // edit, or a lookup would never be allowed to answer.
        form->setProperty("username", QStringLiteral("pavel"));
        form->setProperty("mountPoint", QStringLiteral("/home/pavel/DATA"));
        check(QStringLiteral("host initialisation is not a user edit"),
              !form->property("credentialsSealed").toBool());

        const bool applied = applySuggestion(form, QStringLiteral("nasuser"),
                                             QStringLiteral("WORKGROUP"),
                                             QStringLiteral("synthetic-secret"));
        check(QStringLiteral("a suggestion fills an untouched form"),
              applied && textOf(form, "userField") == QStringLiteral("nasuser")
                  && textOf(form, "domainField") == QStringLiteral("WORKGROUP")
                  && textOf(form, "passwordField") == QStringLiteral("synthetic-secret"),
              textOf(form, "userField"));

        // End of the chain: what was imported is what is submitted, through
        // the same call typed input goes through.
        const int before = actions.calls;
        callMethod(form, "submit");
        check(QStringLiteral("an imported credential is submitted like a typed one"),
              actions.calls == before + 1 && actions.lastUsername == QStringLiteral("nasuser")
                  && actions.lastDomain == QStringLiteral("WORKGROUP")
                  && actions.lastPassword == QStringLiteral("synthetic-secret"),
              actions.lastUsername);
        check(QStringLiteral("submitting seals the credential fields"),
              form->property("credentialsSealed").toBool());
        check(QStringLiteral("a suggestion arriving after submit is refused"),
              !applySuggestion(form, QStringLiteral("other"), QString(),
                               QStringLiteral("other-secret")));
        delete form;
    }

    // --- an edit in any one field rejects the whole candidate ---------------
    {
        struct EditCase {
            const char *field;
            QString typed;
            QString label;
        };
        const EditCase cases[] = {
            {"userField", QStringLiteral("typed-user"),
             QStringLiteral("a typed username rejects a late candidate")},
            {"passwordField", QStringLiteral("typed-secret"),
             QStringLiteral("a typed password rejects a late candidate")},
            {"domainField", QStringLiteral("TYPEDDOM"),
             QStringLiteral("a typed domain rejects a late candidate")},
        };
        for (const EditCase &testCase : cases) {
            QObject *form = freshForm();
            if (!form) {
                return 1;
            }
            form->setProperty("username", QStringLiteral("pavel"));
            simulateEdit(form, testCase.field, testCase.typed);
            check(QStringLiteral("%1 (sealed)").arg(testCase.label),
                  form->property("credentialsSealed").toBool());
            const bool applied = applySuggestion(form, QStringLiteral("nasuser"),
                                                 QStringLiteral("WORKGROUP"),
                                                 QStringLiteral("synthetic-secret"));
            // The strong half of the rule: not only is the suggestion
            // refused, nothing of it reaches any field. A password paired
            // with a typed username is the failure this test exists for.
            check(testCase.label,
                  !applied && textOf(form, testCase.field) == testCase.typed
                      && textOf(form, "passwordField") != QStringLiteral("synthetic-secret")
                      && textOf(form, "domainField") != QStringLiteral("WORKGROUP"),
                  QStringLiteral("user=%1 domain=%2")
                      .arg(textOf(form, "userField"), textOf(form, "domainField")));
            delete form;
        }
    }

    // --- guest selection ----------------------------------------------------
    {
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        form->setProperty("username", QStringLiteral("pavel"));
        // Clearing the username is how a user asks for guest access.
        simulateEdit(form, "userField", QString());
        const bool applied = applySuggestion(form, QStringLiteral("nasuser"), QString(),
                                             QStringLiteral("synthetic-secret"));
        check(QStringLiteral("clearing the username keeps guest state"),
              !applied && textOf(form, "userField").isEmpty()
                  && textOf(form, "passwordField").isEmpty()
                  && textOf(form, "domainField").isEmpty(),
              textOf(form, "userField"));
        delete form;
    }
    {
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        form->setProperty("username", QStringLiteral("pavel"));
        // A candidate with no username must not empty the field and so
        // silently switch the user to guest.
        const bool applied = applySuggestion(form, QString(), QString(),
                                             QStringLiteral("synthetic-secret"));
        check(QStringLiteral("an empty-username candidate changes nothing"),
              !applied && textOf(form, "userField") == QStringLiteral("pavel")
                  && textOf(form, "passwordField").isEmpty());
        delete form;
    }

    // --- a mount-point edit is not a credential edit ------------------------
    {
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        form->setProperty("username", QStringLiteral("pavel"));
        form->setProperty("mountPoint", QStringLiteral("/home/pavel/Elsewhere"));
        check(QStringLiteral("changing only the mount point still accepts a candidate"),
              applySuggestion(form, QStringLiteral("nasuser"), QString(),
                              QStringLiteral("synthetic-secret")));
        delete form;
    }

    // --- the host's binding, not an assignment ------------------------------
    // The service-menu window does not assign the initial username; it binds
    // it (`username: backend.suggestedUser`). A binding that survived the
    // suggestion would silently restore the local login over the imported
    // account name, so instantiate the form the way its real host does and
    // check that the suggestion wins.
    {
        const QString directory = QFileInfo(QStringLiteral(NASMOUNT_SHAREFORM_QML)).absolutePath();
        QQmlComponent host(&engine);
        host.setData(QByteArrayLiteral("import QtQuick\n"
                                       "import \".\"\n"
                                       "Item {\n"
                                       "    property string suggestedUser: \"pavel\"\n"
                                       "    ShareForm { objectName: \"form\"; username: parent.suggestedUser }\n"
                                       "}\n"),
                     QUrl::fromLocalFile(directory + QStringLiteral("/host_under_test.qml")));
        QObject *wrapper = host.create();
        if (!wrapper) {
            out << "  FAIL  the host wrapper does not load   " << host.errorString() << Qt::endl;
            return 1;
        }
        QObject *form = wrapper->findChild<QObject *>(QStringLiteral("form"));
        check(QStringLiteral("the bound initial username reaches the field"),
              form && textOf(form, "userField") == QStringLiteral("pavel"),
              form ? textOf(form, "userField") : QStringLiteral("<no form>"));
        const bool applied = applySuggestion(form, QStringLiteral("nasuser"), QString(),
                                             QStringLiteral("synthetic-secret"));
        check(QStringLiteral("a suggestion replaces a bound initial username"),
              applied && textOf(form, "userField") == QStringLiteral("nasuser"),
              textOf(form, "userField"));
        delete wrapper;
    }

    // --- reset(), which the KCM's Add dialog relies on ----------------------
    {
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        simulateEdit(form, "passwordField", QStringLiteral("typed-secret"));
        callMethod(form, "reset");
        check(QStringLiteral("reset clears the fields"),
              textOf(form, "userField").isEmpty() && textOf(form, "passwordField").isEmpty()
                  && textOf(form, "domainField").isEmpty());
        check(QStringLiteral("reset clears the seal, so a reused form is usable again"),
              !form->property("credentialsSealed").toBool());
        delete form;
    }

    out << (failed == 0 ? "all passed" : "FAILURES") << ": " << passed << " passed, " << failed
        << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}

#include "shareform_qml_test.moc"
