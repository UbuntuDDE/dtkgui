/*
 * Copyright (C) 2019 ~ 2020 Deepin Technology Co., Ltd.
 *
 * Author:     sunkang <sunkang@uniontech.com>
 *
 * Maintainer: sunkang <sunkang@uniontech.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dtaskbarcontrol.h"

#include <QVariant>
#include <QGuiApplication>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QDebug>

#include "private/dtaskbarcontrol_p.h"

DCORE_USE_NAMESPACE
DGUI_BEGIN_NAMESPACE

DTaskbarControlPrivate::DTaskbarControlPrivate(DTaskbarControl *q)
    : DObjectPrivate(q)
    , m_counter(0)
    , m_counterVisible(true)
    , m_progress(0)
    , m_progressVisible(true)
{

}

DTaskbarControlPrivate::~DTaskbarControlPrivate()
{

}

/*!
 * \~chinese class DTaskbarControl
 * \~chinese brief DTaskbarControl提供了一个Launcher API接口，用于方便应用程序控制taskbar进度条,设置当前任务数量
 * \~chinese 如果想隐藏或者不使用进度条和任务数的显示，可以使用setProgress,setCounter 传递false隐藏相关显示
 */

DTaskbarControl::DTaskbarControl(QObject *parent)
    : QObject(parent)
    , DObject(*new DTaskbarControlPrivate(this))
{

}

DTaskbarControl::~DTaskbarControl()
{

}

/*!
 * \~chinese brief DTaskbarControl::setProgress 设置当前进度和进度条是否可见
 * \~chinese param progressVisible true可见 false不可见
 * \~chinese param progress 当前进度值 0-1(换算成百分比)
 */
void DTaskbarControl::setProgress(bool progressVisible, double progress)
{
    if (!qFuzzyCompare(d_func()->m_progress, progress)) {
        d_func()->m_progress = progress;
        Q_EMIT progressChanged(progress);
    }

    if (d_func()->m_progressVisible != progressVisible) {
        d_func()->m_progressVisible = progressVisible;
        Q_EMIT progressVisibleChanged(progressVisible);
    }

    QVariantMap properties;
    properties.insert("progress-visible", progressVisible);
    properties.insert("progress", progress);
    sendMessage(properties);
}

/*!
 * \~chinese brief DTaskbarControl::setCounter 设置当前任务数量
 * \~chinese param counterVisible true任务数可见 false任务数不可见
 * \~chinese param counter
 */
void DTaskbarControl::setCounter(bool counterVisible, int counter)
{
    if (counter != d_func()->m_counter) {
        d_func()->m_counter = counter;
        Q_EMIT counterChanged(counter);
    }

    if (d_func()->m_counterVisible != counterVisible) {
        d_func()->m_counterVisible = counterVisible;
        Q_EMIT counterVisibleChanged(counterVisible);
    }

    QVariantMap properties;
    properties.insert("count-visible", counterVisible);
    properties.insert("count", counter);
    sendMessage(properties);
}

/*!
 * \~chinese brief DTaskbarControl::counter 获取当前任务数
 * \~chinese return 任务数量
 */
int DTaskbarControl::counter() const
{
    return d_func()->m_counter;
}

/*!
 * \~chinese brief DTaskbarControl::setCounterVisible 设置任务数是否可见
 * \~chinese param counterVisible true可见 false不可见
 * \~chinese 如果需要隐藏,建议设置counter的值为0并且设置为false，只设置false，有可能会显示
 */
void DTaskbarControl::setCounterVisible(bool counterVisible)
{
    if (d_func()->m_counterVisible != counterVisible) {
        d_func()->m_counterVisible = counterVisible;
        Q_EMIT counterVisibleChanged(counterVisible);
    }

    QVariantMap properties;
    properties.insert("count-visible", counterVisible);
    sendMessage(properties);
}

/*!
 * \~chinese brief DTaskbarControl::counterVisible 返回任务数是否可见
 * \~chinese return true可见 false不可见
 */
bool DTaskbarControl::counterVisible() const
{
    return d_func()->m_counterVisible;
}

/*!
 * \~chinese brief DTaskbarControl::setUrgency 设置任务的紧急程度
 * \~chinese param val true 任务紧急 false普通任务
 */
void DTaskbarControl::setUrgency(bool val)
{
    QVariantMap properties;
    properties.insert("urgent", val);
    sendMessage(properties);
}

void DTaskbarControl::sendMessage(const QVariantMap &params)
{
    if (QGuiApplication::desktopFileName().isEmpty()) {
        qWarning() << "You need to set the desktop file name before you can use DTaskbarControl!";
        return;
    }

    auto message = QDBusMessage::createSignal("/com/deepin/dtkgui/DTaskbarControl",
                                              "com.canonical.Unity.LauncherEntry",
                                              "Update");

    message << "application://" + QGuiApplication::desktopFileName()
            << params;
    QDBusConnection::sessionBus().send(message);
}


DGUI_END_NAMESPACE
