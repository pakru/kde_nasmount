/*
 * Tests that the shipped Russian catalog, built from po/ in this tree, is the
 * one every layer that translates actually reads.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The rest of the suite runs in English (every test pins LANGUAGE=en_US), so a
 * layer compiled without the shared catalog name would pass all of it while
 * its text stayed English for every user. Three layers can go wrong
 * independently, and each is checked against the same catalog:
 *
 *  - this file, which stands for the dialog's and the KCM's C++, built with
 *    the catalog name the targets share;
 *  - the session library, which the KCM runs inside System Settings, where a
 *    call without the macro would look in System Settings' own catalog;
 *  - ShareForm.qml, which resolves i18n() from its host's context in both
 *    front ends.
 *
 * The catalog is taken from the build tree rather than from an installed
 * location, so this proves the .mo that ships, on a machine that has
 * installed nothing. It needs a real locale: glibc's gettext ignores the
 * requested language under C and C.UTF-8, which is all a package build
 * container has, so there the test reports itself skipped (exit 77, which
 * CTest is told to treat as skipped) instead of failing on English text it
 * could never have translated.
 */

#include "mountmodel.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>

#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <QVariant>

#include <clocale>

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

/** The least a host must offer for ShareForm to load with a fixed share: it
 *  shows the share through displayUrl() and asks for the mount point's
 *  verdict while the bindings evaluate. Nothing here is under test. */
class StubActions : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE QString displayUrl(const QString &unc) const { return unc; }
    Q_INVOKABLE QString mountPointProblem(const QString &) const { return QString(); }
};

/** Whether anything under the form shows exactly this text; the labels carry
 *  no objectName, so they are found by what they say. */
bool showsText(QObject *form, const QString &text)
{
    const auto children = form->findChildren<QObject *>();
    for (const QObject *child : children) {
        if (child->property("text").toString() == text) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    // No window is shown; offscreen keeps this runnable without a display and
    // Basic keeps the form style-independent, as in shareform_qml_test.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);

    QTextStream out(stdout);
    out << "i18n_test" << Qt::endl;

    // Asked after the application exists: Qt is what adopts the environment's
    // locale. A locale that could not be set (en_US.UTF-8 not installed) reads
    // as C here as well, which is the right answer: nothing would translate.
    const QByteArray messagesLocale(setlocale(LC_MESSAGES, nullptr));
    if (messagesLocale == "C" || messagesLocale == "POSIX" || messagesLocale.startsWith("C.")) {
        out << "SKIPPED: the message locale is \"" << messagesLocale
            << "\", and gettext ignores the requested language under it; run under a "
               "real UTF-8 locale such as en_US.UTF-8"
            << Qt::endl;
        return 77;
    }

    KLocalizedString::addDomainLocaleDir(QByteArrayLiteral(TRANSLATION_DOMAIN),
                                         QStringLiteral(NASMOUNT_BUILD_LOCALE_DIR));
    KLocalizedString::setLanguages({QStringLiteral("ru")});

    // --- the catalog is found -----------------------------------------------
    // The same call mountmodel.cpp makes for this label, so the entry is the
    // shipped one. English here means the .mo was not found or lacks the entry.
    const QString mounted = i18nc("@item:inlistbox share state", "Mounted");
    check(QStringLiteral("a catalog message is translated"),
          !mounted.isEmpty() && mounted != QStringLiteral("Mounted"), mounted);

    // --- the session library uses the same catalog ----------------------------
    // Compiled into its own static library with its own copy of the catalog
    // name: if that definition were lost its text would be System Settings'
    // domain's, i.e. English, while the call above is Russian.
    {
        Session::RowPresentInput input;
        input.state = Session::DisplayState::Mounted;
        const QString shown = Session::presentRow(input).stateText;
        check(QStringLiteral("the session library reads the shared catalog"),
              shown == mounted, QStringLiteral("library: %1   catalog: %2").arg(shown, mounted));
    }

    // --- ShareForm under a context with the shared domain ----------------------
    {
        QQmlEngine engine;
        // As the dialog's main() and KCMUtils do for their engines: it takes
        // the catalog name from TRANSLATION_DOMAIN, and without it the form's
        // i18n() would not exist.
        KLocalization::setupLocalizedContext(&engine);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(NASMOUNT_SHAREFORM_QML)));
        if (component.isError()) {
            check(QStringLiteral("ShareForm.qml loads"), false, component.errorString());
            return 1;
        }
        StubActions actions;
        QObject *form = component.create();
        if (!form) {
            check(QStringLiteral("ShareForm.qml can be instantiated"), false, component.errorString());
            return 1;
        }
        form->setProperty("actions", QVariant::fromValue(static_cast<QObject *>(&actions)));
        form->setProperty("fixedUnc", QStringLiteral("//nas.example/DATA"));

        const QString english = QStringLiteral("Mount point:");
        const QString label = i18nc("@label:textbox", "Mount point:");
        check(QStringLiteral("the form's label is translated by the shared catalog"),
              label != english && showsText(form, label), label);
        delete form;
    }

    out << (failed == 0 ? "all passed" : "FAILURES") << ": " << passed << " passed, " << failed
        << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}

#include "i18n_test.moc"
