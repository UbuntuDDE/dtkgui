/*
 * Copyright (C) 2021 ~ 2021 Deepin Technology Co., Ltd.
 *
 * Author:     Chen Bin <chenbin@uniontech.com>
 *
 * Maintainer: Chen Bin <chenbin@uniontech.com>
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
#include <gtest/gtest.h>
#include <QWidget>
#include <QWindow>
#include <QTest>
#include <QDebug>
#include <QBuffer>

#include "dplatformhandle.h"

#define DXCB_PLUGIN_KEY "dxcb"
#define DXCB_PLUGIN_SYMBOLIC_PROPERTY "_d_isDxcb"
#define SETWMWALLPAPERPARAMETER "_d_setWmWallpaperParameter"
#define CLIENTLEADER "_d_clientLeader"
#define WINDOWRADIUS "_d_windowRadius"
#define BORDERWIDTH "_d_borderWidth"
#define BORDRCOLOR "_d_borderColor"
#define SHADOWRADIUS "_d_shadowRadius"
#define SHADOWOFFSET "_d_shadowOffset"
#define SHADOWCOLOR "_d_shadowColor"
#define CLIPPATH "_d_clipPath"
#define FRAMEMASK "_d_frameMask"
#define FRAMEMARGINS "_d_frameMargins"
#define TRANSLUCENTBACKGROUND "_d_translucentBackground"
#define ENABLESYSTEMRESIZE "_d_enableSystemResize"
#define ENABLESYSTEMMOVE "_d_enableSystemMove"
#define ENABLEBLURWINDOW "_d_enableBlurWindow"
#define AUTOINPUTMASKBYCLIPPATH "_d_autoINPUTMASKBYCLIPPATH"
#define REALWINDOWID "_d_real_content_window"

DGUI_USE_NAMESPACE

class TDPlatformHandle : public testing::Test
{
protected:
    void SetUp();
    void TearDown();

    QWidget *widget;
    DPlatformHandle *pHandle;
};

void TDPlatformHandle::SetUp()
{
    widget = new QWidget;
    widget->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(widget));

    if (QWindow *wHandle = widget->windowHandle()) {
        pHandle = new DPlatformHandle(wHandle);
        ASSERT_TRUE(pHandle);
    }
}

void TDPlatformHandle::TearDown()
{
    delete widget;
    delete pHandle;
}

TEST_F(TDPlatformHandle, testFunction)
{
    if (!pHandle || qgetenv("QT_QPA_PLATFORM").contains("offscreen"))
        return;

    EXPECT_FALSE(DPlatformHandle::pluginVersion().isEmpty());
    EXPECT_EQ(DPlatformHandle::isDXcbPlatform(), (qApp->platformName() == DXCB_PLUGIN_KEY || qApp->property(DXCB_PLUGIN_SYMBOLIC_PROPERTY).toBool()));
    (DPlatformHandle::enableDXcbForWindow(widget->windowHandle()));
    (DPlatformHandle::enableDXcbForWindow(widget->windowHandle(), true));

    qInfo() << "TDPlatformHandle(isEnabledDXcb):" << DPlatformHandle::isEnabledDXcb(widget->windowHandle());

    EXPECT_TRUE(DPlatformHandle::setEnabledNoTitlebarForWindow(widget->windowHandle(), true));
    EXPECT_TRUE(DPlatformHandle::setEnabledNoTitlebarForWindow(widget->windowHandle(), false));

    QVector<DPlatformHandle::WMBlurArea> wmAreaVector;
    wmAreaVector << dMakeWMBlurArea(0, 0, 20, 20, 4, 4);

    EXPECT_TRUE(pHandle->setWindowBlurAreaByWM(widget->windowHandle(),  wmAreaVector));

    QPainterPath pPath;
    pPath.addRect({0, 0, 20, 20});

    EXPECT_TRUE(pHandle->setWindowBlurAreaByWM(widget->windowHandle(), {pPath}));
    EXPECT_TRUE(pHandle->setWindowBlurAreaByWM(wmAreaVector));
    EXPECT_TRUE(pHandle->setWindowBlurAreaByWM({pPath}));

    if (qApp->platformFunction(SETWMWALLPAPERPARAMETER)) {
        EXPECT_TRUE(pHandle->setWindowWallpaperParaByWM(widget->windowHandle(), {0, 0, 20, 20}, DPlatformHandle::FollowScreen, DPlatformHandle::PreserveAspectFit));
    } else {
        EXPECT_FALSE(pHandle->setWindowWallpaperParaByWM(widget->windowHandle(), {0, 0, 20, 20}, DPlatformHandle::FollowScreen, DPlatformHandle::PreserveAspectFit));
    }


    if (qApp->platformFunction(CLIENTLEADER)) {
        ASSERT_TRUE(DPlatformHandle::windowLeader());
    } else {
        ASSERT_FALSE(DPlatformHandle::windowLeader());
    }

    DPlatformHandle::setDisableWindowOverrideCursor(widget->windowHandle(), true);
    QVariant windowRadius = widget->windowHandle()->property(WINDOWRADIUS);

    if (windowRadius.isValid() && windowRadius.canConvert(QVariant::Int)) {
        ASSERT_EQ(pHandle->windowRadius(), windowRadius.toInt());
    }

    QVariant borderWidth = widget->windowHandle()->property(BORDERWIDTH);

    if (borderWidth.isValid() && borderWidth.canConvert(QVariant::Int)) {
        ASSERT_EQ(pHandle->borderWidth(), borderWidth.toInt());
    } else {
        ASSERT_EQ(pHandle->borderWidth(), 0);
    }

    QVariant borderColor = widget->windowHandle()->property(BORDRCOLOR);

    if (borderColor.isValid() && borderColor.canConvert(QVariant::Color)) {
        ASSERT_EQ(pHandle->borderColor(), borderColor.value<QColor>());
    } else {
        ASSERT_FALSE(pHandle->borderColor().isValid());
    }

    QVariant shadowRadius = widget->windowHandle()->property(SHADOWRADIUS);

    if (shadowRadius.isValid() && shadowRadius.canConvert(QVariant::Int)) {
        ASSERT_EQ(pHandle->shadowRadius(), shadowRadius.toInt());
    } else {
        ASSERT_FALSE(pHandle->borderColor().isValid());
    }

    QVariant shadowOffset = widget->windowHandle()->property(SHADOWOFFSET);

    if (shadowOffset.isValid() && shadowOffset.canConvert(QVariant::Point)) {
        ASSERT_EQ(pHandle->shadowOffset(), shadowOffset.value<QPoint>());
    } else {
        ASSERT_TRUE(pHandle->shadowOffset().isNull());
    }

    QVariant shadowColor = widget->windowHandle()->property(SHADOWCOLOR);

    if (shadowColor.isValid() && shadowColor.canConvert(QVariant::Color)) {
        ASSERT_EQ(pHandle->shadowColor(), shadowColor.value<QColor>());
    } else {
        ASSERT_FALSE(pHandle->shadowColor().isValid());
    }

    QVariant clipPath = widget->windowHandle()->property(CLIPPATH);

    if (clipPath.isValid() && !clipPath.value<QPainterPath>().isEmpty()) {
        ASSERT_EQ(pHandle->clipPath(), clipPath.value<QPainterPath>());
    } else {
        ASSERT_TRUE(pHandle->clipPath().isEmpty());
    }

    QVariant frameMask = widget->windowHandle()->property(FRAMEMASK);

    if (frameMask.isValid() && frameMask.canConvert(QVariant::Region)) {
        ASSERT_EQ(pHandle->frameMask(), frameMask.value<QRegion>());
    } else {
        ASSERT_TRUE(pHandle->frameMask().isEmpty());
    }

    QVariant frameMargins = widget->windowHandle()->property(FRAMEMARGINS);

    if (frameMargins.isValid() && !frameMargins.value<QMargins>().isNull()) {
        ASSERT_EQ(pHandle->frameMargins(), frameMargins.value<QMargins>());
    } else {
        ASSERT_TRUE(pHandle->frameMargins().isNull());
    }

    QVariant translucentBackground = widget->windowHandle()->property(TRANSLUCENTBACKGROUND);
    if (translucentBackground.isValid() && translucentBackground.canConvert(QVariant::Bool)) {
        ASSERT_EQ(pHandle->translucentBackground(), translucentBackground.toBool());
    } else {
        ASSERT_FALSE(pHandle->translucentBackground());
    }

    QVariant enableSystemResize = widget->windowHandle()->property(ENABLESYSTEMRESIZE);
    if (enableSystemResize.isValid() && enableSystemResize.canConvert(QVariant::Bool)) {
        ASSERT_EQ(pHandle->enableSystemResize(), enableSystemResize.toBool());
    } else {
        ASSERT_FALSE(pHandle->enableSystemResize());
    }

    QVariant enableSystemMove = widget->windowHandle()->property(ENABLESYSTEMMOVE);
    if (enableSystemMove.isValid() && enableSystemMove.canConvert(QVariant::Bool)) {
        ASSERT_EQ(pHandle->enableSystemMove(), enableSystemMove.toBool());
    } else {
        ASSERT_FALSE(pHandle->enableSystemMove());
    }

    QVariant enableBlurWindow = widget->windowHandle()->property(ENABLEBLURWINDOW);
    if (enableBlurWindow.isValid() && enableBlurWindow.canConvert(QVariant::Bool)) {
        ASSERT_EQ(pHandle->enableBlurWindow(), enableBlurWindow.toBool());
    } else {
        ASSERT_FALSE(pHandle->enableBlurWindow());
    }

    QVariant autoInputMaskByClipPath = widget->windowHandle()->property(AUTOINPUTMASKBYCLIPPATH);
    if (autoInputMaskByClipPath.isValid() && autoInputMaskByClipPath.canConvert(QVariant::Bool)) {
        ASSERT_EQ(pHandle->autoInputMaskByClipPath(), autoInputMaskByClipPath.toBool());
    } else {
        ASSERT_FALSE(pHandle->autoInputMaskByClipPath());
    }

    QVariant realWindowId = widget->windowHandle()->property(REALWINDOWID);
    if (enableBlurWindow.isValid() && enableBlurWindow.value<WId>() != 0) {
        ASSERT_EQ(pHandle->realWindowId(), enableBlurWindow.value<WId>());
    } else {
        ASSERT_FALSE(pHandle->realWindowId());
    }
}

TEST_F(TDPlatformHandle, testSlots)
{
    enum { TESTBORDERWIDTH = 4, TESTOFFSET = 6, TESTRADIUS = 8 };
    if (pHandle) {
        pHandle->setWindowRadius(TESTRADIUS);
        ASSERT_EQ(pHandle->windowRadius(), TESTRADIUS);

        pHandle->setBorderWidth(TESTBORDERWIDTH);
        ASSERT_EQ(pHandle->borderWidth(), TESTBORDERWIDTH);

        pHandle->setBorderColor(Qt::black);
        ASSERT_EQ(pHandle->borderColor(), Qt::black);

        pHandle->setShadowRadius(TESTRADIUS);
        ASSERT_EQ(pHandle->shadowRadius(), TESTRADIUS);

        pHandle->setShadowOffset({TESTOFFSET, TESTOFFSET});
        ASSERT_EQ(pHandle->shadowOffset(), QPoint(TESTOFFSET, TESTOFFSET));

        pHandle->setShadowColor(Qt::blue);
        ASSERT_EQ(pHandle->shadowColor(), Qt::blue);

        QPainterPath pPath;
        pPath.addRect({0, 0, 20, 20});

        pHandle->setClipPath(pPath);
        ASSERT_EQ(pHandle->clipPath(), pPath);

        pHandle->setFrameMask(QRegion(0, 0, 10, 10));
        ASSERT_EQ(pHandle->frameMask(), QRegion(0, 0, 10, 10));

        pHandle->setTranslucentBackground(true);
        ASSERT_TRUE(pHandle->translucentBackground());

        pHandle->setEnableSystemResize(true);
        ASSERT_TRUE(pHandle->enableSystemResize());

        pHandle->setEnableSystemMove(true);
        ASSERT_TRUE(pHandle->enableSystemMove());

        pHandle->setEnableBlurWindow(true);
        ASSERT_TRUE(pHandle->enableBlurWindow());

        pHandle->setAutoInputMaskByClipPath(true);
        ASSERT_TRUE(pHandle->autoInputMaskByClipPath());
    }
}

TEST_F(TDPlatformHandle, wmAreaDebug)
{
    DPlatformHandle::WMBlurArea area = dMakeWMBlurArea(0, 0, 20, 20);

    QByteArray data;
    QBuffer buf(&data);
    ASSERT_TRUE(buf.open(QIODevice::WriteOnly));

    QDebug tDebug(&buf);
    tDebug << area;
    buf.close();
    ASSERT_FALSE(data.isEmpty());
}
