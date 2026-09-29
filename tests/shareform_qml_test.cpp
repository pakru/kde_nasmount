/*
 * Tests for ShareForm.qml - the *real* file from the source tree, not a C++
 * model of it: its credential rules, its smb:// input, and its read-only
 * mode (the KCM's Details view).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One rule cannot be checked anywhere else: an imported credential must never
 * be paired with a manually entered one. Both halves of it
 * live in QML, which resolves nothing at compile time, so a C++ imitation
 * would keep passing while the form drifted. Edits are simulated by emitting
 * the field's own textEdited signal, and assignment is used where the host
 * assigns, which pins the distinction the feature rests on.
 */

#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTextStream>
#include <QUrl>
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

    /** Stand-ins for the two pure lookups the form makes. The real ones are
     *  covered by shareaddress_test; these need only be faithful enough to
     *  drive the form's own rules. */
    Q_INVOKABLE QString displayUrl(const QString &unc) const
    {
        return unc.startsWith(QStringLiteral("//")) ? QStringLiteral("smb:") + unc : unc;
    }

    Q_INVOKABLE QString userInShareInput(const QString &text) const
    {
        if (!text.startsWith(QStringLiteral("smb://"))) {
            return QString();
        }
        const QString authority = text.mid(6).section(QLatin1Char('/'), 0, 0);
        const bool hasShare = !text.mid(6).section(QLatin1Char('/'), 1).isEmpty();
        return (hasShare && authority.contains(QLatin1Char('@'))) ? authority.section(QLatin1Char('@'), 0, 0)
                                                                  : QString();
    }

    /** Stand-ins for the two verdicts the form displays. The real rules are
     *  covered by shareaddress_test and mountactions_test; these need only be
     *  faithful enough to drive the form's own behaviour - when a problem is
     *  shown, what it gates, and how a late answer is applied. */
    Q_INVOKABLE QString shareInputProblem(const QString &input, const QString &username) const
    {
        Q_UNUSED(username)
        QString rest = input;
        if (rest.startsWith(QStringLiteral("smb://"), Qt::CaseInsensitive)) {
            rest.remove(0, 6);
        } else if (rest.startsWith(QStringLiteral("//"))) {
            rest.remove(0, 2);
        } else if (!rest.isEmpty()) {
            return QStringLiteral("Not a share address (expected smb://host/share)");
        }
        return rest.isEmpty() ? QStringLiteral("Enter the share address, for example smb://nas/Media") : QString();
    }

    Q_INVOKABLE QString mountPointProblem(const QString &raw) const
    {
        const QString text = raw.trimmed();
        if (text.isEmpty()) {
            return QStringLiteral("Choose a folder to mount the share on");
        }
        return (text.startsWith(QLatin1Char('/')) || text.startsWith(QLatin1Char('~')))
            ? QString()
            : QStringLiteral("Mount point must be an absolute path");
    }

    /** Stand-ins for the Share browser's conversions; the real ones are covered
     *  by shareaddress_test and mountactions_test. */
    Q_INVOKABLE QVariantMap browsedShareInput(const QUrl &picked) const
    {
        if (picked.scheme() != QStringLiteral("smb")) {
            return {{QStringLiteral("input"), QString()},
                    {QStringLiteral("user"), QString()},
                    {QStringLiteral("error"), QStringLiteral("Choose a folder on a network share (smb://)")}};
        }
        QString path = picked.path();
        while (path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }
        const QString input = QStringLiteral("smb://") + picked.host() + path;
        return {{QStringLiteral("input"), input},
                {QStringLiteral("user"), picked.userName()},
                {QStringLiteral("error"), QString()}};
    }

    Q_INVOKABLE QUrl browseStartUrl(const QString &text) const
    {
        return text.length() > 6 ? QUrl(text) : QUrl(QStringLiteral("smb://"));
    }

    /** The server and the first path component, as the real one keys them. */
    Q_INVOKABLE QString lookupTargetOf(const QString &text) const
    {
        QString rest = text;
        if (rest.startsWith(QStringLiteral("smb://"), Qt::CaseInsensitive)) {
            rest.remove(0, 6);
        } else if (rest.startsWith(QStringLiteral("//"))) {
            rest.remove(0, 2);
        }
        const QStringList parts = rest.split(QLatin1Char('/'));
        if (parts.size() < 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty()) {
            return QString();
        }
        return QStringLiteral("smb://") + parts.at(0).toLower() + QLatin1Char('/') + parts.at(1);
    }

    /** Records the request; the test decides when, and whether, it is answered. */
    Q_INVOKABLE void checkMountPoint(const QString &raw)
    {
        ++checks;
        lastChecked = raw;
    }

    /** Delivers a worker-thread answer, as MountActions would. */
    void answerCheck(const QString &raw, const QString &problem)
    {
        Q_EMIT mountPointChecked(raw, problem);
    }

Q_SIGNALS:
    void mountPointChecked(const QString &raw, const QString &problem);

public:
    /** Stand-in for the folder-picker conversion; the real one is covered by
     *  mountactions_test. Faithful enough to drive the form: a local file URL
     *  gives its path, anything else gives nothing. */
    Q_INVOKABLE QString localPathFromUrl(const QUrl &url) const
    {
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }

    int calls = 0;
    int checks = 0;
    QString lastChecked;
    QString lastUnc;
    QString lastMountPoint;
    QString lastUsername;
    QString lastDomain;
    QString lastPassword;
    QString lastAccess;
};

/** Collects the form's credentialLookupRequested signal, which is declared in
 *  QML and so is connected by name. */
class LookupRecorder : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    void onRequested(const QString &shareInput, const QString &user)
    {
        ++count;
        lastShare = shareInput;
        lastUser = user;
    }

public:
    int count = 0;
    QString lastShare;
    QString lastUser;
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

QVariant propertyOf(QObject *form, const char *name, const char *property)
{
    QObject *object = fieldNamed(form, name);
    return object ? object->property(property) : QVariant();
}

void showDefinition(QObject *form, const QVariantMap &values)
{
    QMetaObject::invokeMethod(form, "showDefinition", Q_ARG(QVariant, QVariant(values)));
}

bool radioChecked(QObject *form, const char *name)
{
    return propertyOf(form, name, "checked").toBool();
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
    // account, so instantiate the form the way its real host does - with the
    // share fixed, as the service menu always has it.
    {
        const QString directory = QFileInfo(QStringLiteral(NASMOUNT_SHAREFORM_QML)).absolutePath();
        QQmlComponent host(&engine);
        host.setData(QByteArrayLiteral("import QtQuick\n"
                                       "import \".\"\n"
                                       "Item {\n"
                                       "    property string suggestedUser: \"alice\"\n"
                                       "    ShareForm { objectName: \"form\"; fixedUnc: \"//nas/DATA\"; username: parent.suggestedUser }\n"
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

    // --- the Add form's share address (the KCM, no fixed share) -------------
    auto freshAddForm = [&freshForm]() -> QObject * {
        QObject *form = freshForm();
        if (form) {
            form->setProperty("fixedUnc", QString());
            callMethod(form, "reset");
        }
        return form;
    };
    {
        QObject *form = freshAddForm();
        if (!form) {
            return 1;
        }
        check(QStringLiteral("reset pre-fills smb://"), textOf(form, "uncField") == QStringLiteral("smb://"),
              textOf(form, "uncField"));
        form->setProperty("mountPoint", QStringLiteral("/home/user/DATA"));
        check(QStringLiteral("the bare smb:// prefix is not submittable"), !form->property("canSubmit").toBool());
        simulateEdit(form, "uncField", QStringLiteral("//"));
        check(QStringLiteral("a bare // prefix is not submittable"), !form->property("canSubmit").toBool());
        simulateEdit(form, "uncField", QStringLiteral("smb://nas/DATA"));
        check(QStringLiteral("a share after the prefix is submittable"), form->property("canSubmit").toBool());
        delete form;
    }
    {
        QObject *form = freshAddForm();
        if (!form) {
            return 1;
        }
        simulateEdit(form, "uncField", QStringLiteral("smb://alice@nas/DATA"));
        check(QStringLiteral("the address's user fills an empty Username"),
              textOf(form, "userField") == QStringLiteral("alice"), textOf(form, "userField"));
        check(QStringLiteral("filling from the address is not a credential edit"),
              !form->property("credentialsSealed").toBool());
        simulateEdit(form, "uncField", QStringLiteral("smb://alicia@nas/DATA"));
        check(QStringLiteral("a corrected address updates the Username it filled"),
              textOf(form, "userField") == QStringLiteral("alicia"), textOf(form, "userField"));
        simulateEdit(form, "passwordField", QStringLiteral("typed-secret"));
        simulateEdit(form, "uncField", QStringLiteral("smb://nas/DATA"));
        check(QStringLiteral("an address losing its user never clears Username or the password"),
              textOf(form, "userField") == QStringLiteral("alicia")
                  && textOf(form, "passwordField") == QStringLiteral("typed-secret"));
        delete form;
    }
    {
        QObject *form = freshAddForm();
        if (!form) {
            return 1;
        }
        simulateEdit(form, "userField", QStringLiteral("bob"));
        simulateEdit(form, "uncField", QStringLiteral("smb://alice@nas/DATA"));
        check(QStringLiteral("the address never overwrites a typed Username"),
              textOf(form, "userField") == QStringLiteral("bob"), textOf(form, "userField"));
        delete form;
    }

    // --- read-only mode: the KCM's Details view -----------------------------
    auto freshDetails = [&freshAddForm](const QVariantMap &values) -> QObject * {
        QObject *form = freshAddForm();
        if (form) {
            form->setProperty("readOnly", true);
            showDefinition(form, values);
        }
        return form;
    };
    {
        QObject *form = freshDetails({{QStringLiteral("remoteUrl"), QStringLiteral("smb://nas/DATA")},
                                      {QStringLiteral("mountPoint"), QStringLiteral("/home/user/DATA")},
                                      {QStringLiteral("authentication"), QStringLiteral("credentials")},
                                      {QStringLiteral("username"), QStringLiteral("alice")},
                                      {QStringLiteral("domain"), QStringLiteral("WORKGROUP")},
                                      {QStringLiteral("access"), QStringLiteral("readonly")}});
        if (!form) {
            return 1;
        }
        check(QStringLiteral("details: the saved values are shown"),
              textOf(form, "uncField") == QStringLiteral("smb://nas/DATA")
                  && form->property("mountPoint").toString() == QStringLiteral("/home/user/DATA")
                  && textOf(form, "userField") == QStringLiteral("alice")
                  && textOf(form, "domainField") == QStringLiteral("WORKGROUP"));
        check(QStringLiteral("details: the saved access mode is the one checked"),
              radioChecked(form, "readOnlyRadio") && !radioChecked(form, "readWriteRadio")
                  && !radioChecked(form, "executableRadio"));
        check(QStringLiteral("details: the password is never shown"), textOf(form, "passwordField").isEmpty());
        check(QStringLiteral("details: text fields are read-only"),
              propertyOf(form, "uncField", "readOnly").toBool() && propertyOf(form, "userField", "readOnly").toBool()
                  && propertyOf(form, "passwordField", "readOnly").toBool()
                  && propertyOf(form, "domainField", "readOnly").toBool());
        check(QStringLiteral("details: access radios are disabled"),
              !propertyOf(form, "readOnlyRadio", "enabled").toBool()
                  && !propertyOf(form, "readWriteRadio", "enabled").toBool()
                  && !propertyOf(form, "executableRadio", "enabled").toBool());
        check(QStringLiteral("details: Browse is hidden"), !propertyOf(form, "browseButton", "visible").toBool());
        check(QStringLiteral("details: never submittable"), !form->property("canSubmit").toBool());
        const int callsBefore = actions.calls;
        callMethod(form, "submit");
        check(QStringLiteral("details: submit() records no call"), actions.calls == callsBefore);
        check(QStringLiteral("details: sealed against suggestions"),
              form->property("credentialsSealed").toBool()
                  && !applySuggestion(form, QStringLiteral("mallory"), QString(), QStringLiteral("x")));
        // The password itself is never shown (checked above); a credentials
        // share still shows that one exists, rather than an empty field that
        // would read as "no password" or an "Unknown" that would read as a
        // missing credential.
        const QString passwordPlaceholder = propertyOf(form, "passwordField", "placeholderText").toString();
        check(QStringLiteral("details: a credentials share shows that a password exists"),
              !passwordPlaceholder.isEmpty() && passwordPlaceholder != QStringLiteral("Unknown"),
              passwordPlaceholder);
        delete form;
    }
    {
        QObject *form = freshDetails({{QStringLiteral("remoteUrl"), QStringLiteral("smb://nas/PUBLIC")},
                                      {QStringLiteral("authentication"), QStringLiteral("guest")},
                                      {QStringLiteral("access"), QStringLiteral("readwrite")}});
        if (!form) {
            return 1;
        }
        check(QStringLiteral("details: a guest share says so"),
              textOf(form, "userField").isEmpty()
                  && propertyOf(form, "userField", "placeholderText").toString().contains(QStringLiteral("guest")),
              propertyOf(form, "userField", "placeholderText").toString());
        delete form;
    }
    {
        // An orphan share has credentials but no saved username: its empty
        // Username must not read as guest.
        QObject *form = freshDetails({{QStringLiteral("remoteUrl"), QStringLiteral("smb://nas/DATA")},
                                      {QStringLiteral("authentication"), QStringLiteral("credentials")},
                                      {QStringLiteral("access"), QStringLiteral("readwrite")}});
        if (!form) {
            return 1;
        }
        const QString placeholder = propertyOf(form, "userField", "placeholderText").toString();
        check(QStringLiteral("details: an unknown username is unknown, not guest"),
              placeholder.startsWith(QStringLiteral("Unknown")) && !placeholder.contains(QStringLiteral("guest")),
              placeholder);
        delete form;
    }
    {
        QObject *form = freshDetails({{QStringLiteral("remoteUrl"), QStringLiteral("smb://nas/DATA")},
                                      {QStringLiteral("authentication"), QString()},
                                      {QStringLiteral("access"), QString()}});
        if (!form) {
            return 1;
        }
        check(QStringLiteral("details: unknown authentication reads Unknown"),
              propertyOf(form, "userField", "placeholderText").toString() == QStringLiteral("Unknown"));
        check(QStringLiteral("details: an unknown access mode checks no radio"),
              !radioChecked(form, "readOnlyRadio") && !radioChecked(form, "readWriteRadio")
                  && !radioChecked(form, "executableRadio"));
        delete form;
    }

    // --- the mount point's Browse result -------------------------------------
    {
        QObject *form = freshAddForm();
        if (!form) {
            return 1;
        }
        auto browse = [form](const QUrl &url) {
            QMetaObject::invokeMethod(form, "applyBrowsedMountPoint", Q_ARG(QVariant, QVariant(url)));
        };

        // A folder named "100%" is the case that used to arrive as "100%25".
        browse(QUrl::fromLocalFile(QStringLiteral("/home/user/100%")));
        check(QStringLiteral("browse: a % in the folder name reaches the field literally"),
              textOf(form, "pathField") == QStringLiteral("/home/user/100%"),
              textOf(form, "pathField"));
        browse(QUrl::fromLocalFile(QStringLiteral("/home/user/a#b c")));
        check(QStringLiteral("browse: a # and a space reach the field literally"),
              textOf(form, "pathField") == QStringLiteral("/home/user/a#b c"),
              textOf(form, "pathField"));
        check(QStringLiteral("browse: a good pick shows no message"),
              form->property("mountBrowseError").toString().isEmpty()
                  && !propertyOf(form, "mountPointMessage", "visible").toBool());

        // KDE's dialog can also be pointed at smb://; that is not a mount point.
        browse(QUrl(QStringLiteral("smb://nas/Media")));
        check(QStringLiteral("browse: a network location leaves the field unchanged"),
              textOf(form, "pathField") == QStringLiteral("/home/user/a#b c"),
              textOf(form, "pathField"));
        check(QStringLiteral("browse: a network location says why"),
              form->property("mountBrowseError").toString() == QStringLiteral("Choose a local folder")
                  && propertyOf(form, "mountPointMessage", "visible").toBool());

        simulateEdit(form, "pathField", QStringLiteral("/home/user/typed"));
        check(QStringLiteral("browse: typing in the field clears the message"),
              form->property("mountBrowseError").toString().isEmpty());

        browse(QUrl(QStringLiteral("smb://nas/Media")));
        browse(QUrl::fromLocalFile(QStringLiteral("/home/user/ok")));
        check(QStringLiteral("browse: a later good pick clears the message"),
              form->property("mountBrowseError").toString().isEmpty()
                  && textOf(form, "pathField") == QStringLiteral("/home/user/ok"));

        browse(QUrl(QStringLiteral("smb://nas/Media")));
        callMethod(form, "reset");
        check(QStringLiteral("browse: reset clears the message for a reused form"),
              form->property("mountBrowseError").toString().isEmpty());
        delete form;
    }

    // --- validation: what is shown, when, and what it gates ------------------
    {
        auto shown = [](QObject *form, const char *message) {
            return propertyOf(form, message, "visible").toBool();
        };
        auto editingFinished = [](QObject *form, const char *field) {
            QMetaObject::invokeMethod(fieldNamed(form, field), "editingFinished");
        };

        const int base = actions.checks; // the stub is shared with the sections above
        // Pristine: the pre-filled prefix and the empty path are not mistakes.
        QObject *form = freshAddForm();
        if (!form) {
            return 1;
        }
        check(QStringLiteral("validation: a fresh form scolds nothing"),
              !shown(form, "shareMessage") && !shown(form, "mountPointMessage"));
        check(QStringLiteral("validation: ... and cannot be submitted"), !form->property("canSubmit").toBool());

        // Typing a bad share is not scolded until it is finished.
        simulateEdit(form, "uncField", QStringLiteral("nas/share"));
        check(QStringLiteral("validation: a bad share is not shown while it is being typed"),
              !shown(form, "shareMessage"));
        check(QStringLiteral("validation: ... but already gates Add"),
              form->property("shareProblem").toString().contains(QStringLiteral("Not a share address")));
        editingFinished(form, "uncField");
        check(QStringLiteral("validation: leaving the field shows the problem"),
              shown(form, "shareMessage")
                  && textOf(form, "shareMessage").contains(QStringLiteral("Not a share address")),
              textOf(form, "shareMessage"));
        simulateEdit(form, "uncField", QStringLiteral("smb://nas/DATA"));
        check(QStringLiteral("validation: a fixed value clears the message at once"), !shown(form, "shareMessage"));
        simulateEdit(form, "uncField", QStringLiteral("smb://"));
        check(QStringLiteral("validation: and a new problem shows live once the field has been revealed"),
              shown(form, "shareMessage")
                  && textOf(form, "shareMessage").contains(QStringLiteral("Enter the share address")));
        simulateEdit(form, "uncField", QStringLiteral("smb://nas/DATA"));

        // Mount point.
        editingFinished(form, "pathField");
        check(QStringLiteral("validation: leaving an empty Mount point asks for one"),
              shown(form, "mountPointMessage")
                  && textOf(form, "mountPointMessage").contains(QStringLiteral("Choose a folder")));
        check(QStringLiteral("validation: an empty path is never sent to the worker"), actions.checks == base);
        simulateEdit(form, "pathField", QStringLiteral("relative/dir"));
        check(QStringLiteral("validation: a lexical problem shows and gates Add"),
              shown(form, "mountPointMessage") && !form->property("canSubmit").toBool());
        editingFinished(form, "pathField");
        check(QStringLiteral("validation: ... and the worker is not asked about it"), actions.checks == base);

        simulateEdit(form, "pathField", QStringLiteral("/home/user/DATA"));
        check(QStringLiteral("validation: a good path clears the message and enables Add"),
              !shown(form, "mountPointMessage") && form->property("canSubmit").toBool());
        editingFinished(form, "pathField");
        check(QStringLiteral("validation: leaving the field asks the worker about that text"),
              actions.checks == base + 1 && actions.lastChecked == QStringLiteral("/home/user/DATA"),
              actions.lastChecked);
        check(QStringLiteral("validation: an unanswered check does not block Add"),
              form->property("canSubmit").toBool());

        // The worker's answer.
        actions.answerCheck(QStringLiteral("/home/user/DATA"), QStringLiteral("Folder is not empty"));
        check(QStringLiteral("validation: an answer for the current text is shown"),
              shown(form, "mountPointMessage")
                  && textOf(form, "mountPointMessage") == QStringLiteral("Folder is not empty"));
        check(QStringLiteral("validation: ... and gates Add"), !form->property("canSubmit").toBool());
        actions.answerCheck(QStringLiteral("/home/user/OTHER"), QStringLiteral("stale answer"));
        check(QStringLiteral("validation: an answer for other text is ignored"),
              textOf(form, "mountPointMessage") == QStringLiteral("Folder is not empty"));
        simulateEdit(form, "pathField", QStringLiteral("/home/user/DATA2"));
        check(QStringLiteral("validation: editing the path forgets the old answer"),
              !shown(form, "mountPointMessage") && form->property("canSubmit").toBool());
        actions.answerCheck(QStringLiteral("/home/user/DATA"), QStringLiteral("Folder is not empty"));
        check(QStringLiteral("validation: a late answer for the old text does not come back"),
              !shown(form, "mountPointMessage"));

        // A pick is finished input.
        callMethod(form, "reset");
        check(QStringLiteral("validation: reset makes the form pristine again"),
              !shown(form, "shareMessage") && !shown(form, "mountPointMessage")
                  && !form->property("shareRevealed").toBool() && !form->property("mountPointRevealed").toBool());
        const int before = actions.checks;
        QMetaObject::invokeMethod(form, "applyBrowsedMountPoint",
                                  Q_ARG(QVariant, QVariant(QUrl::fromLocalFile(QStringLiteral("/home/user/Picked")))));
        check(QStringLiteral("validation: a Browse pick is checked at once"),
              actions.checks == before + 1 && actions.lastChecked == QStringLiteral("/home/user/Picked"),
              actions.lastChecked);
        actions.answerCheck(QStringLiteral("/home/user/Picked"), QStringLiteral("Something is already mounted"));
        check(QStringLiteral("validation: and its answer shows without waiting for a pause"),
              shown(form, "mountPointMessage"));
        delete form;

        // The service menu fixes the share and pre-fills the path.
        form = freshForm();
        if (!form) {
            return 1;
        }
        form->setProperty("mountPoint", QStringLiteral("/home/user/DATA"));
        check(QStringLiteral("validation: a fixed share never gates on the address"),
              form->property("shareProblem").toString().isEmpty() && form->property("canSubmit").toBool());
        delete form;

        // A host that pre-fills the path as the form is created.
        {
            engine.rootContext()->setContextProperty(QStringLiteral("testActions"), &actions);
            const QString directory = QFileInfo(QStringLiteral(NASMOUNT_SHAREFORM_QML)).absolutePath();
            QQmlComponent host(&engine);
            host.setData(QByteArrayLiteral("import QtQuick\n"
                                           "import \".\"\n"
                                           "Item {\n"
                                           "    ShareForm { objectName: \"form\"; actions: testActions;\n"
                                           "                fixedUnc: \"//nas/DATA\"; mountPoint: \"/home/user/Suggested\" }\n"
                                           "}\n"),
                         QUrl::fromLocalFile(directory + QStringLiteral("/host_prefill_under_test.qml")));
            const int checksBefore = actions.checks;
            QObject *wrapper = host.create();
            QObject *prefilled = wrapper ? wrapper->findChild<QObject *>(QStringLiteral("form")) : nullptr;
            check(QStringLiteral("validation: a path the host pre-fills is checked as the form appears"),
                  prefilled && actions.checks == checksBefore + 1
                      && actions.lastChecked == QStringLiteral("/home/user/Suggested"),
                  wrapper ? actions.lastChecked : host.errorString());
            check(QStringLiteral("validation: ... and its answer shows immediately"),
                  prefilled && (actions.answerCheck(QStringLiteral("/home/user/Suggested"), QStringLiteral("Folder is not empty")),
                                shown(prefilled, "mountPointMessage")));
            delete wrapper;
        }

        // The Details view of a saved share is not validated.
        QObject *details = freshAddForm();
        if (!details) {
            return 1;
        }
        details->setProperty("readOnly", true);
        showDefinition(details, {{QStringLiteral("remoteUrl"), QStringLiteral("smb://nas/DATA")},
                                 {QStringLiteral("mountPoint"), QStringLiteral("/home/user/DATA")}});
        const int detailsBefore = actions.checks;
        editingFinished(details, "pathField");
        actions.answerCheck(QStringLiteral("/home/user/DATA"), QStringLiteral("Another mount already uses this folder"));
        check(QStringLiteral("validation: a saved share is never checked or scolded"),
              actions.checks == detailsBefore && !shown(details, "mountPointMessage")
                  && !shown(details, "shareMessage") && !details->property("canSubmit").toBool());
        delete details;
    }

    // --- Share Browse ---------------------------------------------------------
    {
        auto shown = [](QObject *form, const char *message) {
            return propertyOf(form, message, "visible").toBool();
        };
        auto browseShare = [](QObject *form, const QString &url) {
            QMetaObject::invokeMethod(form, "applyBrowsedShare", Q_ARG(QVariant, QVariant(QUrl(url))));
        };
        auto watch = [](QObject *form, LookupRecorder *recorder) {
            return QObject::connect(form, SIGNAL(credentialLookupRequested(QString,QString)), recorder,
                                    SLOT(onRequested(QString,QString)));
        };

        QObject *form = freshAddForm();
        if (!form) {
            return 1;
        }
        check(QStringLiteral("share browse: the Add form offers it"),
              propertyOf(form, "shareBrowseButton", "visible").toBool());
        delete form;
        form = freshForm();
        if (!form) {
            return 1;
        }
        check(QStringLiteral("share browse: a fixed share offers none"),
              !propertyOf(form, "shareBrowseButton", "visible").toBool());
        delete form;
        form = freshAddForm();
        if (!form) {
            return 1;
        }
        form->setProperty("readOnly", true);
        check(QStringLiteral("share browse: the Details view offers none"),
              !propertyOf(form, "shareBrowseButton", "visible").toBool());
        delete form;

        // A pick lands in the field as if typed.
        form = freshAddForm();
        if (!form) {
            return 1;
        }
        LookupRecorder recorder;
        check(QStringLiteral("share browse: the form's lookup signal can be observed"), watch(form, &recorder));
        browseShare(form, QStringLiteral("smb://nas.local/DATA/Films"));
        check(QStringLiteral("share browse: the picked share fills the field"),
              textOf(form, "uncField") == QStringLiteral("smb://nas.local/DATA/Films"), textOf(form, "uncField"));
        check(QStringLiteral("share browse: a pick is not a credential edit"),
              !form->property("credentialsSealed").toBool());
        check(QStringLiteral("share browse: a login is asked for that share"),
              recorder.count == 1 && recorder.lastShare == QStringLiteral("smb://nas.local/DATA/Films")
                  && recorder.lastUser.isEmpty(),
              recorder.lastShare);
        check(QStringLiteral("share browse: the field is revealed, a pick being finished input"),
              form->property("shareRevealed").toBool());

        // The user in the picked URL fills Username and stays out of the address.
        browseShare(form, QStringLiteral("smb://alice@nas.local/DATA"));
        check(QStringLiteral("share browse: the picked URL's user fills an empty Username"),
              textOf(form, "userField") == QStringLiteral("alice"), textOf(form, "userField"));
        check(QStringLiteral("share browse: ... and is not left in the address"),
              textOf(form, "uncField") == QStringLiteral("smb://nas.local/DATA"), textOf(form, "uncField"));
        check(QStringLiteral("share browse: ... and constrains the lookup"),
              recorder.count == 2 && recorder.lastUser == QStringLiteral("alice"), recorder.lastUser);
        delete form;

        // A typed Username is the user's; a pick neither overwrites it nor asks for a login.
        form = freshAddForm();
        if (!form) {
            return 1;
        }
        LookupRecorder typedRecorder;
        watch(form, &typedRecorder);
        simulateEdit(form, "userField", QStringLiteral("bob"));
        browseShare(form, QStringLiteral("smb://alice@nas.local/DATA"));
        check(QStringLiteral("share browse: a typed Username is never overwritten"),
              textOf(form, "userField") == QStringLiteral("bob"), textOf(form, "userField"));
        check(QStringLiteral("share browse: ... and a taken-over credential form asks for no login"),
              typedRecorder.count == 0);
        delete form;

        // Something that is not a share on the network changes nothing.
        form = freshAddForm();
        if (!form) {
            return 1;
        }
        LookupRecorder refusedRecorder;
        watch(form, &refusedRecorder);
        simulateEdit(form, "uncField", QStringLiteral("smb://typed/SHARE"));
        browseShare(form, QStringLiteral("file:///home/user/Documents"));
        check(QStringLiteral("share browse: a local folder leaves the field alone"),
              textOf(form, "uncField") == QStringLiteral("smb://typed/SHARE"), textOf(form, "uncField"));
        check(QStringLiteral("share browse: ... says why, under the field"),
              shown(form, "shareMessage") && textOf(form, "shareMessage").contains(QStringLiteral("network share")),
              textOf(form, "shareMessage"));
        check(QStringLiteral("share browse: ... and asks for no login"), refusedRecorder.count == 0);
        simulateEdit(form, "uncField", QStringLiteral("smb://typed/OTHER"));
        check(QStringLiteral("share browse: typing clears that message"), !shown(form, "shareMessage"));
        delete form;

        // reset() leaves nothing of a previous pick.
        form = freshAddForm();
        if (!form) {
            return 1;
        }
        browseShare(form, QStringLiteral("file:///home/user/Documents"));
        callMethod(form, "reset");
        check(QStringLiteral("share browse: reset clears the message for a reused form"),
              !shown(form, "shareMessage") && form->property("shareBrowseError").toString().isEmpty());
        delete form;
    }

    // --- an imported login belongs to the share it was found for --------------
    {
        auto browseShare = [](QObject *form, const QString &url) {
            QMetaObject::invokeMethod(form, "applyBrowsedShare", Q_ARG(QVariant, QVariant(QUrl(url))));
        };
        // Browses to a share and delivers a login for it, as the KCM would.
        auto importedForm = [&](LookupRecorder *recorder) -> QObject * {
            QObject *form = freshAddForm();
            if (!form) {
                return nullptr;
            }
            QObject::connect(form, SIGNAL(credentialLookupRequested(QString,QString)), recorder,
                             SLOT(onRequested(QString,QString)));
            browseShare(form, QStringLiteral("smb://nas.local/DATA"));
            return form;
        };
        auto identity = [](QObject *form) {
            return QStringList{textOf(form, "userField"), textOf(form, "domainField"),
                               textOf(form, "passwordField")};
        };
        const QStringList imported = {QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"),
                                      QStringLiteral("synthetic-secret")};
        const QStringList nothing = {QString(), QString(), QString()};

        LookupRecorder recorder;
        QObject *form = importedForm(&recorder);
        if (!form) {
            return 1;
        }
        const bool appliedFirst = applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"),
                                                  QStringLiteral("synthetic-secret"));
        check(QStringLiteral("imported: a login for the requested share is applied"),
              appliedFirst && identity(form) == imported, identity(form).join(QStringLiteral("|")));
        check(QStringLiteral("imported: ... and is not a credential edit"),
              !form->property("credentialsSealed").toBool());

        // The same share, one level deeper: still that share's login.
        simulateEdit(form, "uncField", QStringLiteral("smb://nas.local/DATA/Films"));
        check(QStringLiteral("imported: a subfolder of the same share keeps it"), identity(form) == imported,
              identity(form).join(QStringLiteral("|")));
        simulateEdit(form, "uncField", QStringLiteral("smb://NAS.local/DATA"));
        check(QStringLiteral("imported: the host's letter case does not matter"), identity(form) == imported);

        // Another server, or another share: the login does not travel.
        simulateEdit(form, "uncField", QStringLiteral("smb://other.local/DATA"));
        check(QStringLiteral("imported: another server takes the whole login back"), identity(form) == nothing,
              identity(form).join(QStringLiteral("|")));
        check(QStringLiteral("imported: ... without counting as a credential edit"),
              !form->property("credentialsSealed").toBool());
        check(QStringLiteral("imported: ... and nothing is left marked as imported"),
              form->property("importedTarget").toString().isEmpty()
                  && !form->property("importedPassword").toBool() && !form->property("importedIdentity").toBool());
        delete form;

        form = importedForm(&recorder);
        applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"), QStringLiteral("synthetic-secret"));
        simulateEdit(form, "uncField", QStringLiteral("smb://nas.local/OTHER"));
        check(QStringLiteral("imported: another share on the same server takes it back too"),
              identity(form) == nothing, identity(form).join(QStringLiteral("|")));
        delete form;

        // Half-imported: what the user made their own stays theirs.
        form = importedForm(&recorder);
        applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"), QStringLiteral("synthetic-secret"));
        simulateEdit(form, "passwordField", QStringLiteral("mine"));
        simulateEdit(form, "uncField", QStringLiteral("smb://other.local/DATA"));
        check(QStringLiteral("imported: a password the user retyped survives a move"),
              textOf(form, "passwordField") == QStringLiteral("mine"), textOf(form, "passwordField"));
        delete form;

        form = importedForm(&recorder);
        applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"), QStringLiteral("synthetic-secret"));
        simulateEdit(form, "userField", QStringLiteral("someone-else"));
        simulateEdit(form, "uncField", QStringLiteral("smb://other.local/DATA"));
        check(QStringLiteral("imported: the imported password never follows a changed share, whoever edited what"),
              textOf(form, "passwordField").isEmpty(), textOf(form, "passwordField"));
        check(QStringLiteral("imported: ... while the username the user typed stays"),
              textOf(form, "userField") == QStringLiteral("someone-else"), textOf(form, "userField"));
        delete form;

        // A late answer about an address that is gone.
        form = importedForm(&recorder);
        simulateEdit(form, "uncField", QStringLiteral("smb://other.local/DATA"));
        const bool lateApplied = applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"),
                                                 QStringLiteral("synthetic-secret"));
        check(QStringLiteral("imported: an answer for an address no longer in the field is refused"),
              !lateApplied && identity(form) == nothing, identity(form).join(QStringLiteral("|")));
        check(QStringLiteral("imported: ... and an answer nobody asked for is refused"), [&]() {
            QObject *quiet = freshAddForm();
            const bool refused = quiet && !applySuggestion(quiet, QStringLiteral("nasuser"), QString(),
                                                          QStringLiteral("synthetic-secret"));
            delete quiet;
            return refused;
        }());
        delete form;

        // An answer after the user began typing credentials.
        form = importedForm(&recorder);
        simulateEdit(form, "passwordField", QStringLiteral("typed"));
        check(QStringLiteral("imported: an answer after a credential edit is refused"),
              !applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"),
                               QStringLiteral("synthetic-secret")));
        delete form;

        // Not sealed by the move, so the next pick can ask again.
        form = importedForm(&recorder);
        applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"), QStringLiteral("synthetic-secret"));
        simulateEdit(form, "uncField", QStringLiteral("smb://other.local/DATA"));
        const int asked = recorder.count;
        browseShare(form, QStringLiteral("smb://other.local/DATA"));
        check(QStringLiteral("imported: after a move, picking again asks for a login again"),
              recorder.count == asked + 1 && recorder.lastShare == QStringLiteral("smb://other.local/DATA"),
              recorder.lastShare);
        delete form;

        // reset() forgets it all.
        form = importedForm(&recorder);
        applySuggestion(form, QStringLiteral("nasuser"), QStringLiteral("WORKGROUP"), QStringLiteral("synthetic-secret"));
        callMethod(form, "reset");
        check(QStringLiteral("imported: reset forgets what was imported and what was asked"),
              form->property("importedTarget").toString().isEmpty() && form->property("lookupShare").toString().isEmpty()
                  && identity(form) == nothing);
        delete form;

        // The service menu: the share is fixed, so none of this applies.
        form = freshForm();
        if (!form) {
            return 1;
        }
        check(QStringLiteral("imported: a fixed share accepts a login without a request"),
              applySuggestion(form, QStringLiteral("nasuser"), QString(), QStringLiteral("synthetic-secret")));
        check(QStringLiteral("imported: ... and never marks anything for withdrawal"),
              form->property("importedTarget").toString().isEmpty());
        delete form;
    }

    // --- the access labels ---------------------------------------------------
    {
        // The same three literals mountmodel_test pins for accessModeLabel():
        // the list's access column and this form must name a mode alike.
        QObject *form = freshForm();
        if (!form) {
            return 1;
        }
        check(QStringLiteral("access radio labels match the list's access column"),
              propertyOf(form, "readOnlyRadio", "text").toString() == QStringLiteral("Read only")
                  && propertyOf(form, "readWriteRadio", "text").toString() == QStringLiteral("Read & Write")
                  && propertyOf(form, "executableRadio", "text").toString()
                      == QStringLiteral("Read & Write & Execute"));
        delete form;
    }

    out << (failed == 0 ? "all passed" : "FAILURES") << ": " << passed << " passed, " << failed
        << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}

#include "shareform_qml_test.moc"
