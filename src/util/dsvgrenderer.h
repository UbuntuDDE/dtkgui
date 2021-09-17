/*
 * Copyright (C) 2017 ~ 2017 Deepin Technology Co., Ltd.
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

#ifndef DSVGRENDERER_H
#define DSVGRENDERER_H

#include <dtkgui_global.h>
#include <DObject>

#include <QObject>
#include <QRectF>

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE

#ifdef Q_OS_LINUX
DGUI_BEGIN_NAMESPACE
class DSvgRendererPrivate;
class DSvgRenderer : public QObject, public DTK_CORE_NAMESPACE::DObject
{
    Q_PROPERTY(QRectF viewBox READ viewBoxF WRITE setViewBox)
public:
    explicit DSvgRenderer(QObject *parent = Q_NULLPTR);
    DSvgRenderer(const QString &filename, QObject *parent = Q_NULLPTR);
    DSvgRenderer(const QByteArray &contents, QObject *parent = Q_NULLPTR);
    ~DSvgRenderer();

    bool isValid() const;

    QSize defaultSize() const;

    QRect viewBox() const;
    QRectF viewBoxF() const;
    void setViewBox(const QRect &viewbox);
    void setViewBox(const QRectF &viewbox);

    QRectF boundsOnElement(const QString &id) const;
    bool elementExists(const QString &id) const;

    QImage toImage(const QSize sz, const QString &elementId = QString()) const;

public Q_SLOTS:
    bool load(const QString &filename);
    bool load(const QByteArray &contents);
    void render(QPainter *p);
    void render(QPainter *p, const QRectF &bounds);

    void render(QPainter *p, const QString &elementId,
                const QRectF &bounds = QRectF());

private:
    D_DECLARE_PRIVATE(DSvgRenderer)
};
DGUI_END_NAMESPACE
#else

#include <QSvgRenderer>
DGUI_BEGIN_NAMESPACE
typedef  QSvgRenderer DSvgRenderer;
DGUI_END_NAMESPACE

#endif

#endif // DSVGRENDERER_H
