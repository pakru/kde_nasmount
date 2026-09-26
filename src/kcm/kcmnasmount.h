/*
 * kcm_nasmount — System Settings module: list, add and remove
 * generated network-mount definitions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An "action module": every change (Add or Delete) takes
 * effect immediately through Session::MountActions rather than being staged
 * behind Apply/OK, so buttons() is NoAdditionalButton. There is no in-place
 * Edit: changing a share means removing it and adding it again.
 */

#pragma once

#include "mountactions.h"
#include "mountmodel.h"

#include <KQuickConfigModule>

class KcmNasmount : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(Session::MountModel *shareModel READ shareModel CONSTANT)
    Q_PROPERTY(Session::MountActions *actions READ actions CONSTANT)

public:
    KcmNasmount(QObject *parent, const KPluginMetaData &metaData);
    ~KcmNasmount() override;

    Session::MountModel *shareModel() const;
    Session::MountActions *actions() const;

private:
    Session::MountModel *m_model;
    Session::MountActions *m_actions;
};
