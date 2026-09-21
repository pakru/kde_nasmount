/*
 * Tests for the credential half of ShareForm.qml — the *real* file from the
 * source tree, not a C++ model of it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One rule cannot be checked anywhere else: an imported credential must never
 * be paired with a manually entered one (plan §§5, 8.1). Both halves of it
 * live in QML, which resolves nothing at compile time, so a C++ imitation
 * would keep passing while the form drifted. Edits are simulated by emitting
 * the field's own textEdited signal, and assignment is used where the host
 * assigns, which pins the distinction the feature rests on.
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

/** Stands in for Session::MountActions, recording the tuple the form hands
 *  over: that is how the end of the chain is checked, that an imported
 *  password is submitted through the same call a typed one is. */
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
    // No window is shown: the form is a ColumnLayout and every rule under
    // test is a property and two functions. Offscreen keeps this runnable in
    // a package-build container, Basic keeps it style-independent.
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
        // As the service-menu window does from backend.suggestedUser. This
        // must not look like an edit, or no lookup could ever answer.
        form->setProperty("username", QStringLiteral("alice"));
        form->setProperty("mountPoint", QStringLiteral("/home/user/DATA"));
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

        // What was imported is what is submitted.
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
            form->setProperty("username", QStringLiteral("alice"));
            simulateEdit(form, testCase.field, testCase.typed);
            check(QStringLiteral("%1 (sealed)").arg(testCase.label),
                  form->property("credentialsSealed").toBool());
            const bool applied = applySuggestion(form, QStringLiteral("nasuser"),
                                                 QStringLiteral("WORKGROUP"),
                                                 QStringLiteral("synthetic-secret"));
            // The strong half: nothing of the suggestion reaches any field.
            // A password beside a typed username is the failure in question.
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
        form->setProperty("username", QStringLiteral("alice"));
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
        form->setProperty("username", QStringLiteral("alice"));
        // A candidate with no username must not switch the user to guest.
        const bool applied = applySuggestion(form, QString(), QString(),
                                             QStringLiteral("synthetic-secret"));
        check(QStringLiteral("an empty-username candidate changes nothing"),
              !applied && textOf(form, "userField") == QStringLiteral("alice")
                  && textOf(form, "passwordField").isEmpty());
        delete form;
    }

    // --- a mount-point edit is not a credential edit ------------------------
    {
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        form->setProperty("username", QStringLiteral("alice"));
        form->setProperty("mountPoint", QStringLiteral("/home/user/Elsewhere"));
        check(QStringLiteral("changing only the mount point still accepts a candidate"),
              applySuggestion(form, QStringLiteral("nasuser"), QString(),
                              QStringLiteral("synthetic-secret")));
        delete form;
    }

    // --- the host's binding, not an assignment ------------------------------
    // The window binds the initial username rather than assigning it. A
    // binding that survived would restore the local login over the imported
    // account, so instantiate the form the way its real host does.
    {
        const QString directory = QFileInfo(QStringLiteral(NASMOUNT_SHAREFORM_QML)).absolutePath();
        QQmlComponent host(&engine);
        host.setData(QByteArrayLiteral("import QtQuick\n"
                                       "import \".\"\n"
                                       "Item {\n"
                                       "    property string suggestedUser: \"alice\"\n"
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
              form && textOf(form, "userField") == QStringLiteral("alice"),
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
