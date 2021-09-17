/*
 * Copyright (C) 2019 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
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
#include "dguiapplicationhelper.h"
#include "private/dguiapplicationhelper_p.h"
#include "dplatformhandle.h"

#include <QHash>
#include <QColor>
#include <QPalette>
#include <QWindow>
#include <QGuiApplication>
#include <QPointer>
#include <QPlatformSurfaceEvent>
#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>

#include <private/qguiapplication_p.h>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

DGUI_BEGIN_NAMESPACE

#ifdef QT_DEBUG
Q_LOGGING_CATEGORY(dgAppHelper, "dtk.dguihelper")
#else
Q_LOGGING_CATEGORY(dgAppHelper, "dtk.dguihelper", QtInfoMsg)
#endif

#ifdef Q_OS_LINUX
class DInstanceGuard {
public:
    static bool guard(const QString &name);
    static void enterCriticalSection();
    static void leaveCriticalSection();

    class DCriticalHolder {
    public:
        DCriticalHolder() {
            DInstanceGuard::enterCriticalSection();
        }
        ~DCriticalHolder() {
            DInstanceGuard::leaveCriticalSection();
        }
    };

private:
    static bool setInstanceName(const QString &name);
    static void shmInit();
    static void destroy();
    static void errorExitIf(bool cond, QStringView reason);

    struct SharedVarables {
        pid_t               pid[2];
        pthread_mutex_t     mutex[2];

        struct {
          pid_t             criticalProcessPid;
          pthread_mutex_t   criticalSectionMtx;
        } CriticalSection;
    };

    static DInstanceGuard * s_pSelf;
    static QString          s_name;

    static int              s_shmId;
    static key_t            s_shmKey;

    static QVector<QString> s_procIdPath;
    static SharedVarables * s_pShm;
    static int              s_nLock;

private:
    DInstanceGuard();
    ~DInstanceGuard() = default;
    DInstanceGuard(const DInstanceGuard &) = delete;
    DInstanceGuard &operator=(const DInstanceGuard &) = delete;
    DInstanceGuard(DInstanceGuard &&) = delete;
    DInstanceGuard &operator=(DInstanceGuard &&) = delete;
};

DInstanceGuard  *   DInstanceGuard::s_pSelf  = nullptr;
QString             DInstanceGuard::s_name;

int                 DInstanceGuard::s_shmId  = 0;
key_t               DInstanceGuard::s_shmKey = 0;

QVector<QString>    DInstanceGuard::s_procIdPath;
DInstanceGuard::SharedVarables *  DInstanceGuard::s_pShm = nullptr;
int                 DInstanceGuard::s_nLock  = -1;

/*!
    \internal DInstanceGuard::DInstanceGuard 构造 DInstanceGuard 实例并初始化

    需要预先传入 name 实例名，相同实例名的实例在系统中只能同时存在最好两个。锁是对两个
    pid 的保护，获取到锁才能改写 pid，最终是根据 pid 来判断当前有几个进程、应该启动几个
    进程。传入的 name 应该按照 Session 或 Scope 做区分，比如同一个用户连接多个
    VNC、同一台主机登陆多个用户。
 */
DInstanceGuard::DInstanceGuard()
{
    for (int i = 0; i < 2 && s_nLock == -1; ++i) {
        if (pthread_mutex_trylock(&s_pShm->mutex[i]) != 0) {
            continue;
        }
        s_nLock = i;
        s_pShm->pid[i] = getpid();
        std::atexit(destroy);
    }
    QString criticalProc = QString("/proc/%1").arg(s_pShm->CriticalSection.criticalProcessPid);
    if (s_pShm->CriticalSection.criticalProcessPid && !QFile(criticalProc).exists()) {
        pthread_mutex_unlock(&s_pShm->CriticalSection.criticalSectionMtx);
    }

    errorExitIf(s_nLock == -1, u"Has two instance running.");
}

/*!
    \internal DInstanceGuard::shmInit() 初始化共享内存

    对共享内存进行初始化，存放进程级别的互斥锁、记录进程的 pid。
*/
void DInstanceGuard::shmInit()
{
    s_shmKey = qHash(s_name);
    s_procIdPath.resize(2);

    pthread_mutexattr_t mutexSharedAttr[2] = {};
    for (int i = 0; i < 2; ++i) {
        pthread_mutexattr_setpshared(&mutexSharedAttr[i], PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_settype(&mutexSharedAttr[i], PTHREAD_MUTEX_RECURSIVE_NP);
    }

    // 获取共享内存，如果不存在则创建并初始化
    bool clearFlag = false;
    s_shmId = shmget(s_shmKey, 0, 0666 | IPC_CREAT);
    if (s_shmId == -1) {
        s_shmId = shmget(s_shmKey, sizeof(SharedVarables), 0666 | IPC_CREAT);
        errorExitIf(s_shmId < 0, u"Create share memory failed.");
        clearFlag = true;
    }
    s_pShm = static_cast<SharedVarables*>(shmat(s_shmId, nullptr, 0));
    errorExitIf(!s_pShm, u"Attach share memory failed.");
    if (clearFlag) memset(s_pShm, 0, sizeof(SharedVarables));

    // 对已退出/新创建的进程的锁进行重新初始化，防止　mutex 内部错误
    for (int i = 0; i < 2; ++i) {
        s_procIdPath[i] = QString("/proc/%1").arg(s_pShm->pid[i]);
        if (s_pShm->pid[i] && !QFile(s_procIdPath[i]).exists()) {
            pthread_mutex_destroy(&s_pShm->mutex[i]);
            pthread_mutex_init(&s_pShm->mutex[i], &mutexSharedAttr[i]);
            s_pShm->pid[i] = 0;
        }
        pthread_mutexattr_destroy(&mutexSharedAttr[i]);
    }

    // 对临界区锁的有效性进行检测，重新初始化
    QString criticalProcPath = QString("/proc/%1").arg(s_pShm->CriticalSection.criticalProcessPid);
    if (!s_pShm->CriticalSection.criticalProcessPid || !QFile(criticalProcPath).exists()) {
        // Critical section init
        pthread_mutexattr_t criticalMtxSharedAttr = {};
        pthread_mutexattr_setpshared(&criticalMtxSharedAttr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_settype(&criticalMtxSharedAttr, PTHREAD_MUTEX_RECURSIVE_NP);
        pthread_mutex_init(&s_pShm->CriticalSection.criticalSectionMtx, &criticalMtxSharedAttr);
        pthread_mutexattr_destroy(&criticalMtxSharedAttr);
    }
}

/*!
    \fn DInstanceGuard::setInstanceName(const QString &name) 设置实例名

    根据实例名初始化对应的共享内存
*/
bool DInstanceGuard::setInstanceName(const QString &name)
{
    errorExitIf(name.isEmpty(), u"Set instance name error...");
    if (!s_name.isEmpty()) {
        qCWarning(dgAppHelper, "Set instance name failed. already has a name...");
        return false;
    }
    s_name = name;

    return true;
}

/*!
    \fn DInstanceGuard::guard 创建 DInstanceGuard 实例

    传入 name 实例名，确保和当前 name 相同的实例在系统中只能同时存在不超过两个，确保后续可以完成后者到
    前者的通信过程，最终只保留一个实例。负责完成 DGuiApplicationHelper::setSingleInstance 中的
    第一阶段。
 */
bool DInstanceGuard::guard(const QString &name)
{
    static std::once_flag initFlag;
    bool retValue = false;
    std::call_once(initFlag, [name, &retValue] {
        retValue = setInstanceName(name);
        shmInit();
        s_pSelf = s_pSelf ? s_pSelf : new DInstanceGuard();
    });

    return retValue;
}

/*!
    \internal DInstanceGuard::destroy() 销毁共享内存

    只有在程序正常退出的时候由 atexit 的注册来调用。即使异常退出也不会对下次执行造成影响。
 */
void DInstanceGuard::destroy()
{
    if (!s_pSelf) {
        return;
    }

    bool shmRelease = false;
    int anotherProc = !s_nLock;
    if (!QFile(s_procIdPath[anotherProc]).exists()) {
        shmRelease = true;
        pthread_mutex_unlock(&s_pShm->mutex[anotherProc]);
    }
    pthread_mutex_unlock(&s_pShm->mutex[s_nLock]);
    if (s_pShm->CriticalSection.criticalProcessPid == getpid()) {
        s_pShm->CriticalSection.criticalProcessPid = 0;
        pthread_mutex_unlock(&s_pShm->CriticalSection.criticalSectionMtx);
    }
    if (shmRelease) {
        shmctl(s_shmId, IPC_RMID, nullptr);
    }

    delete s_pSelf;
    s_pSelf = nullptr;
}

/*!
    \fn void DInstanceGuard::enterCriticalSection()

    进入临界区。 按 Guard 传入的 name 进行代码段的保护。不能单独使用，必须先调用 Guard。
    不能用于同一进程内的多线程。根据实例名的 scope，最大可提供系统级的保护。
    确保 DGuiApplicationHelper::setSingleInstance 中的第二阶段正确执行。
*/
void DInstanceGuard::enterCriticalSection() {
    errorExitIf(s_name.isEmpty() || !s_pSelf, u"Enter critical section failed. must set instance name first.");
    if (pthread_mutex_lock(&s_pShm->CriticalSection.criticalSectionMtx) == 0) {
        s_pShm->CriticalSection.criticalProcessPid = getpid();
    }
}

void DInstanceGuard::leaveCriticalSection() {
    pthread_mutex_unlock(&s_pShm->CriticalSection.criticalSectionMtx);
}

void DInstanceGuard::errorExitIf(bool cond, QStringView reason) {
    if (cond) {
        qCWarning(dgAppHelper) << reason << " should exit program.";
        qFatal("Error: DInstanceGuard::errorExitIf.");
    }
}
#endif

Q_GLOBAL_STATIC(QLocalServer, _d_singleServer)
static quint8 _d_singleServerVersion = 1;

#define WINDOW_THEME_KEY "_d_platform_theme"

bool DGuiApplicationHelperPrivate::useInactiveColor = true;
bool DGuiApplicationHelperPrivate::compositingColor = false;
int DGuiApplicationHelperPrivate::waitTime = 3000;

DGuiApplicationHelperPrivate::DGuiApplicationHelperPrivate(DGuiApplicationHelper *qq)
    : DObjectPrivate(qq)
{

}

void DGuiApplicationHelperPrivate::init()
{
    D_Q(DGuiApplicationHelper);

    systemTheme = new DPlatformTheme(0, q);
    // 初始时先将appTheme指定为systtemTheme，在后面合适的地方再初始化appTheme
    appTheme = systemTheme;

    if (qGuiApp) {
        initApplication(qGuiApp);
    } else {
        qAddPreRoutine(staticInitApplication);
    }
}

void DGuiApplicationHelperPrivate::initApplication(QGuiApplication *app)
{
    D_Q(DGuiApplicationHelper);

    q->connect(app, &QGuiApplication::paletteChanged, q, [q, this, app] {
        if (themeType == DGuiApplicationHelper::UnknownType)
            Q_EMIT q->themeTypeChanged(q->toColorType(app->palette()));
    });


    app->connect(systemTheme, &DPlatformTheme::themeNameChanged, app, [this, app] {
        if (appTheme == systemTheme)
            return;

        appTheme->setFallbackProperty(false);
        const QByteArray &theme_name = appTheme->themeName();
        appTheme->setFallbackProperty(true);

        if (!theme_name.isEmpty())
            notifyAppThemeChanged(app);
    });
    app->connect(systemTheme, &DPlatformTheme::activeColorChanged, app, [this, app] {
        if (appTheme == systemTheme)
            return;

        appTheme->setFallbackProperty(false);
        const QColor &active_color = appTheme->activeColor();
        appTheme->setFallbackProperty(true);

        if (!active_color.isValid())
            notifyAppThemeChanged(app, true);
    });
    app->connect(systemTheme, &DPlatformTheme::paletteChanged, app, [this, app] {
        if (appTheme == systemTheme)
            return;

        if (!appTheme->isValid())
            notifyAppThemeChanged(app);
    });

    if (QGuiApplicationPrivate::is_app_running) {
        _q_initApplicationTheme();
    } else {
        // 延后初始化数据，因为在调用 clientLeader 前必须要保证QGuiApplication已经完全构造完成
        q->metaObject()->invokeMethod(q, "_q_initApplicationTheme", Qt::QueuedConnection, Q_ARG(bool, true));
    }
}

void DGuiApplicationHelperPrivate::staticInitApplication()
{
    if (DGuiApplicationHelper *helper = DGuiApplicationHelper::instance()) {
        helper->d_func()->initApplication(qGuiApp);
    }
}

DPlatformTheme *DGuiApplicationHelperPrivate::initWindow(QWindow *window) const
{
    // 如果appTheme还未初始化，应当先初始化appTheme
    if (appTheme == systemTheme) {
        // 此时QGuiApplication必须是已经初始化完成的状态
        Q_ASSERT(QGuiApplicationPrivate::is_app_running);
        // 初始程序级别的主题对象
        const_cast<DGuiApplicationHelperPrivate*>(this)->_q_initApplicationTheme(true);
    }

    DPlatformTheme *theme = new DPlatformTheme(window->winId(), appTheme);
    window->setProperty(WINDOW_THEME_KEY, QVariant::fromValue(theme));
    theme->setParent(window); // 跟随窗口销毁

    auto onWindowThemeChanged = [theme, window] {
        qGuiApp->postEvent(window, new QEvent(QEvent::ThemeChange));
    };

    window->connect(theme, &DPlatformTheme::themeNameChanged, window, onWindowThemeChanged);
    window->connect(theme, &DPlatformTheme::activeColorChanged, window, onWindowThemeChanged);
    window->connect(theme, &DPlatformTheme::paletteChanged, window, onWindowThemeChanged);

    return theme;
}

void DGuiApplicationHelperPrivate::_q_initApplicationTheme(bool notifyChange)
{
    if (appTheme && appTheme != systemTheme)
        return;

    appTheme = new DPlatformTheme(DPlatformHandle::windowLeader(), systemTheme);
    QGuiApplication *app = qGuiApp;
    auto onAppThemeChanged = std::bind(&DGuiApplicationHelperPrivate::notifyAppThemeChanged, this, app, false);
    QObject::connect(appTheme, &DPlatformTheme::themeNameChanged, app, onAppThemeChanged);
    QObject::connect(appTheme, &DPlatformTheme::activeColorChanged, app, onAppThemeChanged);
    QObject::connect(appTheme, &DPlatformTheme::paletteChanged, app, onAppThemeChanged);

    // appTheme在此之前可能由systemTheme所代替被使用，此时在创建appTheme
    // 并初始化之后，应当发送信号通知程序主题的改变
    if (notifyChange && appTheme->isValid()) {
        notifyAppThemeChanged(app);
    }
}

void DGuiApplicationHelperPrivate::notifyAppThemeChanged(QGuiApplication *app, bool ignorePaletteType)
{
    D_Q(DGuiApplicationHelper);

    if (app->testAttribute(Qt::AA_SetPalette)
            || (!ignorePaletteType && paletteType != DGuiApplicationHelper::UnknownType)) {
        return;
    }

    QWindowSystemInterfacePrivate::ThemeChangeEvent event(nullptr);
    QGuiApplicationPrivate::processThemeChanged(&event);
    Q_EMIT q->themeTypeChanged(q->toColorType(app->palette()));
}

class _DGuiApplicationHelper
{
public:
    _DGuiApplicationHelper()
        : helper(nullptr)
    {
    }

    ~_DGuiApplicationHelper()
    {
        if (helper) {
            helper->deleteLater();
            helper = nullptr;
        }
    }

    static DGuiApplicationHelper *defaultCreator()
    {
        return new DGuiApplicationHelper();
    }

    DGuiApplicationHelper *helper;
    static DGuiApplicationHelper::HelperCreator creator;
};

DGuiApplicationHelper::HelperCreator _DGuiApplicationHelper::creator = _DGuiApplicationHelper::defaultCreator;
Q_GLOBAL_STATIC(_DGuiApplicationHelper, _globalHelper)


/*!
 * \~chinese \class DGuiApplicationHelper
 * \~chinese \brief DGuiApplicationHelper 应用程序的 GUI ，如主题、调色板等
 */

/*!
 *
 * \~chinese \enum DGuiApplicationHelper::ColorType
 * \~chinese DGuiApplicationHelper::ColorType 定义了主题类型
 *
 * \~chinese \var DGuiApplicationHelper:ColorType DGuiApplicationHelper::UnknownType
 * \~chinese 未知主题(浅色主题或深色主题)
 *
 * \~chinese \var DGuiApplicationHelper:ColorType DGuiApplicationHelper::LightType
 * \~chinese 浅色主题
 *
 * \~chinese \var DGuiApplicationHelper:ColorType DGuiApplicationHelper::DarkType
 * \~chinese 深色主题
 */

DGuiApplicationHelper::DGuiApplicationHelper()
    : QObject(nullptr)
    , DObject(*new DGuiApplicationHelperPrivate(this))
{

}

void DGuiApplicationHelper::initialize()
{
    D_D(DGuiApplicationHelper);

    d->init();
}

/*!
 * \~chinese \brief DGuiApplicationHelper::registerInstanceCreator创建 DGuiApplicationHelper 对象
 * \~chinese \param creator 函数指针
 * \~chinese \note \row 一定要先调用此函数,再使用 DGuiApplicationHelper::instance()
 */
void DGuiApplicationHelper::registerInstanceCreator(DGuiApplicationHelper::HelperCreator creator)
{
    if (creator == _DGuiApplicationHelper::creator)
        return;

    _DGuiApplicationHelper::creator = creator;

    if (_globalHelper.exists() && _globalHelper->helper) {
        _globalHelper->helper->deleteLater();
        _globalHelper->helper = nullptr;
    }
}

inline static int adjustColorValue(int base, qint8 increment, int max = 255)
{
    return increment > 0 ? (max - base) * increment / 100.0 + base
           : base * (1 + increment / 100.0);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::instance返回 DGuiApplicationHelper 对象
 * \~chinese \return DGuiApplicationHelper对象
 */
DGuiApplicationHelper *DGuiApplicationHelper::instance()
{
    // 单例模式，延迟创建DGuiApplicationHelper
    if (nullptr == _globalHelper->helper) {
        _globalHelper->helper = _DGuiApplicationHelper::creator();
        _globalHelper->helper->initialize();
        // 当QApplication析构时，同时析构DGuiApplicationHelper
        QObject::connect(qApp, &QObject::destroyed, [](){
            if (_globalHelper->helper) {
                _globalHelper->helper->deleteLater();
                _globalHelper->helper = nullptr;
            }
        });
    }

    return _globalHelper->helper;
}

DGuiApplicationHelper::~DGuiApplicationHelper()
{
    D_D(DGuiApplicationHelper);

    if (d->appPalette) {
        delete d->appPalette;
    }
}

/*!
 * \~chinese \brief DGuiApplicationHelper::adjustColor 调整颜色
 * \~chinese \note \row 取值范围均为 -100 ~ 100 ,当三原色参数为-100时，颜色为黑色，参数为100时，颜色为白色.
 * \~chinese 以透明度( alphaFloat )为例,当参数为负数时基础色的 alphaFloat 值减少，现象偏向透明, 参数为正数alphaFloat 值增加，现象偏不透明
 * \~chinese \param base基础色
 * \~chinese \param hueFloat 色调
 * \~chinese \param saturationFloat 饱和度
 * \~chinese \param lightnessFloat 亮度
 * \~chinese \param redFloat 红色
 * \~chinese \param greenFloat 绿色
 * \~chinese \param blueFloat 蓝色
 * \~chinese \param alphaFloat Alpha通道(透明度)
 * \~chinese \return 经过调整的颜色
 */
QColor DGuiApplicationHelper::adjustColor(const QColor &base,
                                          qint8 hueFloat, qint8 saturationFloat, qint8 lightnessFloat,
                                          qint8 redFloat, qint8 greenFloat, qint8 blueFloat, qint8 alphaFloat)
{
    // 按HSL格式调整
    int H, S, L, A;
    base.getHsl(&H, &S, &L, &A);

    H = H > 0 ? adjustColorValue(H, hueFloat, 359) : H;
    S = adjustColorValue(S, saturationFloat);
    L = adjustColorValue(L, lightnessFloat);
    A = adjustColorValue(A, alphaFloat);

    QColor new_color = QColor::fromHsl(H, S, L, A);

    // 按RGB格式调整
    int R, G, B;
    new_color.getRgb(&R, &G, &B);

    R = adjustColorValue(R, redFloat);
    G = adjustColorValue(G, greenFloat);
    B = adjustColorValue(B, blueFloat);

    new_color.setRgb(R, G, B, A);

    return new_color;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::blendColor 将两种颜色混合，合成新的颜色
 * \~chinese \param substrate底层颜色
 * \~chinese \param superstratum上层颜色
 * \~chinese \return 混合颜色
 */
QColor DGuiApplicationHelper::blendColor(const QColor &substrate, const QColor &superstratum)
{
    QColor c2 = superstratum.toRgb();

    if (c2.alpha() >= 255)
        return c2;

    QColor c1 = substrate.toRgb();
    qreal c1_weight = 1 - c2.alphaF();

    int r = c1_weight * c1.red() + c2.alphaF() * c2.red();
    int g = c1_weight * c1.green() + c2.alphaF() * c2.green();
    int b = c1_weight * c1.blue() + c2.alphaF() * c2.blue();

    return QColor(r, g, b, c1.alpha());
}

static QColor light_qpalette[QPalette::NColorRoles] {
    QColor("#414d68"),                  //WindowText
    QColor("#e5e5e5"),                  //Button
    QColor("#e6e6e6"),                  //Light
    QColor("#e5e5e5"),                  //Midlight
    QColor("#e3e3e3"),                  //Dark
    QColor("#e4e4e4"),                  //Mid
    QColor("#414d68"),                  //Text
    Qt::black,                          //BrightText
    QColor("#414d68"),                  //ButtonText
    Qt::white,                          //Base
    QColor("#f8f8f8"),                  //Window
    QColor(0, 0, 0, 0.05 * 255),        //Shadow
    QColor("#0081ff"),                  //Highlight
    Qt::white,                          //HighlightedText
    QColor("#0082fa"),                  //Link
    QColor("#ad4579"),                  //LinkVisited
    QColor(0, 0, 0, 0.03 * 255),        //AlternateBase
    Qt::white,                          //NoRole
    QColor(255, 255, 255, 0.8 * 255),   //ToolTipBase
    Qt::black                           //ToolTipText
};

static QColor dark_qpalette[QPalette::NColorRoles] {
    QColor("#c0c6d4"),                  //WindowText
    QColor("#444444"),                  //Button
    QColor("#484848"),                  //Light
    QColor("#474747"),                  //Midlight
    QColor("#414141"),                  //Dark
    QColor("#434343"),                  //Mid
    QColor("#c0c6d4"),                  //Text
    Qt::white,                          //BrightText
    QColor("#c0c6d4"),                  //ButtonText
    QColor("#282828"),                  //Base
    QColor("#252525"),                  //Window
    QColor(0, 0, 0, 0.05 * 255),        //Shadow
    QColor("#0081ff"),                  //Highlight
    QColor("#F1F6FF"),                  //HighlightedText
    QColor("#0082fa"),                  //Link
    QColor("#ad4579"),                  //LinkVisited
    QColor(0, 0, 0, 0.05 * 255),        //AlternateBase
    Qt::black,                          //NoRole
    QColor(45, 45, 45, 0.8 * 255),      //ToolTipBase
    QColor("#c0c6d4")                   //ToolTipText
};

static QColor light_dpalette[DPalette::NColorTypes] {
    QColor(),                       //NoType
    QColor(0, 0, 0, 255 * 0.03),    //ItemBackground
    QColor("#001A2E"),              //TextTitle
    QColor("#526A7F"),              //TextTips
    QColor("#FF5736"),              //TextWarning
    Qt::white,                      //TextLively
    QColor("#0081FF"),              //LightLively
    QColor("#0081FF"),              //DarkLively
    QColor(0, 0, 0, 0.05 * 255),    //FrameBorder
    QColor(85, 85, 85, 0.4 * 255),  //PlaceholderText
    QColor(0, 0, 0, 0.1 * 255),     //FrameShadowBorder
    QColor(0, 0, 0, 0.1 * 255)      //ObviousBackground
};

static QColor dark_dpalette[DPalette::NColorTypes] {
    QColor(),                           //NoType
    QColor(255, 255, 255, 255 * 0.05),  //ItemBackground
    QColor("#C0C6D4"),                  //TextTitle
    QColor("#6D7C88"),                  //TextTips
    QColor("#9a2f2f"),                  //TextWarning
    Qt::white,                          //TextLively
    QColor("#0059d2"),                  //LightLively
    QColor("#0059d2"),                  //DarkLively
    QColor(255, 255, 255, 0.1 * 255),   //FrameBorder
    QColor(192, 198, 212, 0.4 * 255),   //PlaceholderText
    QColor(0, 0, 0, 0.8 * 255),         //FrameShadowBorder
    QColor(255, 255, 255, 0.1 * 255)    //ObviousBackground
};

/*!
 * \~chinese \brief DGuiApplicationHelper::standardPalett 根据主题获取标准调色板
 * \~chinese \param type 主题枚举值
 * \~chinese \return 调色板
 */
DPalette DGuiApplicationHelper::standardPalette(DGuiApplicationHelper::ColorType type)
{
    static const DPalette *light_palette = nullptr, *dark_palette = nullptr;
    static const DPalette *alpha_light_palette = nullptr, *alpha_dark_palette = nullptr;

    if (type == LightType) {
        if (Q_UNLIKELY(DGuiApplicationHelperPrivate::compositingColor)) {
            if (Q_LIKELY(alpha_light_palette)) {
                return *alpha_light_palette;
            }
        }

        if (Q_LIKELY(light_palette)) {
            return *light_palette;
        }
    } else if (type == DarkType) {
        if (Q_UNLIKELY(DGuiApplicationHelperPrivate::compositingColor)) {
            if (Q_LIKELY(alpha_dark_palette)) {
                return *alpha_dark_palette;
            }
        }

        if (Q_LIKELY(dark_palette)) {
            return *dark_palette;
        }
    } else {
        return DPalette();
    }

    DPalette *pa;
    const QColor *qcolor_list, *dcolor_list;

    if (type == DarkType) {
        pa = new DPalette();

        if (DGuiApplicationHelperPrivate::compositingColor)
            alpha_dark_palette = pa;
        else
            dark_palette = pa;

        qcolor_list = dark_qpalette;
        dcolor_list = dark_dpalette;
    } else {
        pa = new DPalette();

        if (DGuiApplicationHelperPrivate::compositingColor)
            alpha_light_palette = pa;
        else
            light_palette = pa;

        qcolor_list = light_qpalette;
        dcolor_list = light_dpalette;
    }

    for (int i = 0; i < DPalette::NColorRoles; ++i) {
        QPalette::ColorRole role = static_cast<QPalette::ColorRole>(i);
        QColor color = qcolor_list[i];

#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
        if (role == QPalette::PlaceholderText) {
            // 5.15新添加此颜色 这里使用5.11的颜色保证效果与5.11对齐
            color = dcolor_list[DPalette::PlaceholderText];
            continue;
        }
#endif
        // 处理半透明色
        if (DGuiApplicationHelperPrivate::compositingColor) {
            switch (role) {
            case QPalette::Window:
                color = type == LightType ? adjustColor(color, 0, 0, 0, 0, 0, 0, -20) : adjustColor(color, 0, 0, -10, 0, 0, 0, -20);
                break;
            case QPalette::Base:
                color = adjustColor(color, 0, 0, 0, 0, 0, 0, -20);
                break;
            case QPalette::WindowText:
            case QPalette::Text:
                color = adjustColor(color, 0, 0, type == LightType ? -20 : +20, 0, 0, 0, -20);
                break;
            case QPalette::ButtonText:
                color = type == LightType ? adjustColor(color, 0, 0, -20, 0, 0, 0, -20) : adjustColor(color, 0, 0, +20, 0, 0, 0, 0);
                break;
            case QPalette::Button:
            case QPalette::Light:
            case QPalette::Mid:
            case QPalette::Midlight:
            case QPalette::Dark:
                color = adjustColor(color, 0, 0, -20, 0, 0, 0, -40);
                break;
            default:
                break;
            }
        }

        pa->setColor(DPalette::Active, role, color);
        generatePaletteColor(*pa, role, type);
    }

    for (int i = 0; i < DPalette::NColorTypes; ++i) {
        DPalette::ColorType role = static_cast<DPalette::ColorType>(i);
        QColor color = dcolor_list[i];

        // 处理半透明色
        if (DGuiApplicationHelperPrivate::compositingColor) {
            switch (role) {
            case DPalette::ItemBackground:
                color = adjustColor(color, 0, 0, 100, 0, 0, 0, type == LightType ? -80 : -90);
                break;
            case DPalette::TextTitle:
                color = adjustColor(color, 0, 0, -20, 0, 0, 0, -20);
                break;
            case DPalette::TextTips:
                color = type == LightType ? adjustColor(color, 0, 0, -40, 0, 0, 0, -40) : adjustColor(color, 0, 0, +40, 0, 0, 0, -50);
                break;
            default:
                break;
            }
        }

        pa->setColor(DPalette::Active, role, color);
        generatePaletteColor(*pa, role, type);
    }

    return *const_cast<const DPalette*>(pa);
}

template<typename M>
static void generatePaletteColor_helper(DPalette &base, M role, DGuiApplicationHelper::ColorType type)
{
    if (type == DGuiApplicationHelper::UnknownType) {
        type = DGuiApplicationHelper::toColorType(base);
    }

    QColor disable_mask_color, inactive_mask_color;

    if (type == DGuiApplicationHelper::DarkType) {
        disable_mask_color = dark_qpalette[QPalette::Window];
        inactive_mask_color = dark_qpalette[QPalette::Window];
        disable_mask_color.setAlphaF(0.7);
        inactive_mask_color.setAlphaF(0.6);
    } else {
        disable_mask_color = light_qpalette[QPalette::Window];
        inactive_mask_color = light_qpalette[QPalette::Window];
        disable_mask_color.setAlphaF(0.6);
        inactive_mask_color.setAlphaF(0.4);
    }

    const QColor &color = base.color(QPalette::Normal, role);
    base.setColor(QPalette::Disabled, role, DGuiApplicationHelper::blendColor(color, disable_mask_color));

    QPalette::ColorRole qr = static_cast<QPalette::ColorRole>(role);
    if (qr == QPalette::Text) {
        // disable text color olny -60% alpha
        base.setColor(QPalette::Disabled, role, DGuiApplicationHelper::adjustColor(color, 0, 0, 0, 0, 0, 0, -60));
    }

    if (DGuiApplicationHelperPrivate::useInactiveColor)
        base.setColor(QPalette::Inactive, role, DGuiApplicationHelper::blendColor(color, inactive_mask_color));
    else
        base.setColor(QPalette::Inactive, role, color);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::generatePaletteColor 获取调色板颜色
 * \~chinese \param base调色板
 * \~chinese \param \sa roleQPalette::ColorRole()
 * \~chinese \param type主题枚举值
 */
void DGuiApplicationHelper::generatePaletteColor(DPalette &base, QPalette::ColorRole role, DGuiApplicationHelper::ColorType type)
{
    if (role == QPalette::Window) {
        const QBrush &window = base.brush(QPalette::Normal, role);
        base.setBrush(QPalette::Disabled, role, window);
        base.setBrush(QPalette::Inactive, role, window);
        return;
    } else if (role == QPalette::Highlight && toColorType(base) == DarkType) {
        // 暗色模式下的高亮色亮度要降低10%，避免太突兀
        QColor highlight = base.highlight().color();

        if (highlight.isValid()) {
            base.setColor(QPalette::Highlight, adjustColor(highlight, 0, 0, -20, 0, 0, 0, 0));
        }
    }

    generatePaletteColor_helper(base, role, type);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::generatePaletteColor 获取调色板颜色
 * \~chinese \param base调色板
 * \~chinese \param role背景颜色
 * \~chinese \param type主题枚举值
 */
void DGuiApplicationHelper::generatePaletteColor(DPalette &base, DPalette::ColorType role, DGuiApplicationHelper::ColorType type)
{
    generatePaletteColor_helper(base, role, type);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::generatePalette 根据主题的枚举值获取调色板数据
 * \~chinese \param base调色板
 * \~chinese \param type主题的枚举值
 */
void DGuiApplicationHelper::generatePalette(DPalette &base, ColorType type)
{
    // 先判断调色板的色调
    if (type == UnknownType) {
        type = toColorType(base);
    }

    for (int i = 0; i < QPalette::NColorRoles; ++i) {
        QPalette::ColorRole role = static_cast<QPalette::ColorRole>(i);
        generatePaletteColor(base, role, type);
    }

    for (int i = 0; i < QPalette::NColorRoles; ++i) {
        DPalette::ColorType role = static_cast<DPalette::ColorType>(i);
        generatePaletteColor(base, role, type);
    }
}

/*!
 * \~chinese \brief DGuiApplicationHelper::fetchPalette取出主题的调色板
 * \~chinese \param theme主题信息
 * \~chinese \return 调色板信息
 */
DPalette DGuiApplicationHelper::fetchPalette(const DPlatformTheme *theme)
{
    DPalette base_palette;
    const QByteArray theme_name = theme->themeName();
    ColorType type = LightType;

    // 深色主题
    if (theme_name.endsWith("dark")) {
        type = DarkType;
    }

    bool ok = false;
    base_palette = theme->fetchPalette(standardPalette(type), &ok);
    const QColor &active_color = theme->activeColor();

    if (active_color.isValid()) {
        base_palette.setColor(QPalette::Normal, QPalette::Highlight, active_color);

        // ok为true时会整体处理palette的所有颜色
        if (!ok) {
            generatePaletteColor(base_palette, QPalette::Highlight, type);
        }
    }

    if (ok) {
        generatePalette(base_palette, type);
    }

    return base_palette;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setUseInactiveColorGroup设置是否将调色板的颜色改为半透明模式
 * \~chinese 一般用在主窗口背景为透明、模糊的程序中
 * \~chinese \param on 是否开启
 */
void DGuiApplicationHelper::setUseInactiveColorGroup(bool on)
{
    DGuiApplicationHelperPrivate::useInactiveColor = on;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setColorCompositingEnabled设置是否开启混合颜色
 * \~chinese \param on 是否开启
 */
void DGuiApplicationHelper::setColorCompositingEnabled(bool on)
{
    DGuiApplicationHelperPrivate::compositingColor = on;
}

bool DGuiApplicationHelper::isXWindowPlatform()
{
    static bool isX = qGuiApp->platformName() == "xcb" || qGuiApp->platformName() == "dxcb";

    return isX;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::systemTheme返回系统主题
 * \~chinese \return 系统主题
 */
DPlatformTheme *DGuiApplicationHelper::systemTheme() const
{
    D_DC(DGuiApplicationHelper);

    return d->systemTheme;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::applicationTheme返回应用主题对象
 * \~chinese \return 应用主题
 */
DPlatformTheme *DGuiApplicationHelper::applicationTheme() const
{
    D_DC(DGuiApplicationHelper);

    return d->appTheme;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::windowTheme返回 QWindow 主题对象
 * \~chinese \param windowQWindow 对象
 * \~chinese \return QWindow主题
 */
DPlatformTheme *DGuiApplicationHelper::windowTheme(QWindow *window) const
{
    DPlatformTheme *theme = qvariant_cast<DPlatformTheme*>(window->property(WINDOW_THEME_KEY));

    if (theme) {
        return theme;
    }

    D_DC(DGuiApplicationHelper);
    theme = d->initWindow(window);

    return theme;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::applicationPalette返回应用程序调色板
 * \~chinese \return 应用程序调色板
 */
DPalette DGuiApplicationHelper::applicationPalette() const
{
    D_DC(DGuiApplicationHelper);

    if (d->appPalette) {
        return *d->appPalette;
    }

    ColorType type = UnknownType;

    // 如果应用程序自己设置过palette，则以这个palette为基础获取DPalette
    if (qGuiApp && qGuiApp->testAttribute(Qt::AA_SetPalette)) {
        type = toColorType(qGuiApp->palette());
    } else {
        type = d->paletteType;
    }

    // 如果自定义了palette的类型，则直接返回对应的标准DPalette
    if (type != UnknownType) {
        DPalette pa = standardPalette(type);
        const QColor &active_color = d->appTheme->activeColor();

        if (active_color.isValid()) {
            // 应用Active Color
            pa.setColor(QPalette::Normal, QPalette::Highlight, active_color);
            generatePaletteColor(pa, QPalette::Highlight, type);
        }

        return pa;
    }

    return fetchPalette(d->appTheme);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setApplicationPalette设置应用程序调色板
 * \~chinese \param palette调色板
 */
void DGuiApplicationHelper::setApplicationPalette(const DPalette &palette)
{
    D_D(DGuiApplicationHelper);

    if (d->appPalette) {
        if (palette.resolve()) {
            *d->appPalette = palette;
        } else {
            delete d->appPalette;
        }
    } else if (palette.resolve()) {
        d->appPalette = new DPalette(palette);
    } else {
        return;
    }

    d->notifyAppThemeChanged(qGuiApp, true);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::windowPalette
 * \~chinese \param window
 * \~chinese \return 调色板
 */
DPalette DGuiApplicationHelper::windowPalette(QWindow *window) const
{
    D_DC(DGuiApplicationHelper);

    DPlatformTheme *theme = windowTheme(window);

    if (!theme) {
        return applicationPalette();
    }

    return fetchPalette(theme);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::toColorType 获取颜色的明亮度，将其转换为主题类型的枚举值。
 * \~chinese 转换的策略为：先将颜色转换为rgb格式，再根据 Y = 0.299R + 0.587G + 0.114B 的公式
 * \~chinese 计算出颜色的亮度，亮度大于 191 时认为其为浅色，否则认为其为深色。
 * \~chinese \param color 需要转换为主题的类型的颜色
 * \~chinese \return 主题类型的枚举值
 */
DGuiApplicationHelper::ColorType DGuiApplicationHelper::toColorType(const QColor &color)
{
    if (!color.isValid())
        return UnknownType;

    QColor rgb_color = color.toRgb();
    // 获取rgb颜色的亮度(转换为YUV格式)
    float luminance = 0.299 * rgb_color.redF() + 0.587 * rgb_color.greenF() + 0.114 * rgb_color.blueF();

    if (qRound(luminance * 255) > 191) {
        return LightType;
    }

    return DarkType;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::toColorType获取颜色的明亮度，将其转换为主题类型的枚举值。
 * \~chinese \row 返回调色板背景颜色
 * \~chinese \param palette调色板
 * \~chinese \return 主题类型的枚举值
 */
DGuiApplicationHelper::ColorType DGuiApplicationHelper::toColorType(const QPalette &palette)
{
    return toColorType(palette.background().color());
}

/*!
 * \~chinese \brief DGuiApplicationHelper::themeType主题类型
 * \~chinese \row Dpalette::ColorType 针对某一个控件
 * \~chinese \row DGuiApplicationHelper::ColorType 针对整个程序
 * \~chinese \return 主题类型的枚举值
 */
DGuiApplicationHelper::ColorType DGuiApplicationHelper::themeType() const
{
    D_DC(DGuiApplicationHelper);

    if (d->themeType != UnknownType) {
        return d->themeType;
    }

    return qGuiApp ? toColorType(qGuiApp->palette()) : d->themeType;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::paletteType
 * \~chinese \return 主题类型的枚举值
 */
DGuiApplicationHelper::ColorType DGuiApplicationHelper::paletteType() const
{
    D_DC(DGuiApplicationHelper);

    return d->paletteType;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setSingleInstance 设置DGuiApplicationHelper实例
 * \~chinese \param key 实例关键字
 * \~chinese \param singleScope 实例使用范围
 * \~chinese \return 设置是否成功
 * \~chinese \note 此处所用到DGuiApplicationHelperPrivate::waitTime默认值为3000ms，可通过
 * \~chinese \note DGuiApplicationHelper::setSingleInstanceInterval设置
 */
bool DGuiApplicationHelper::setSingleInstance(const QString &key, DGuiApplicationHelper::SingleScope singleScope)
{
    bool new_server = !_d_singleServer.exists();

    if (_d_singleServer->isListening()) {
        _d_singleServer->close();
    }

    QString socket_key = "_d_dtk_single_instance_";

    switch (singleScope) {
    case GroupScope:
        _d_singleServer->setSocketOptions(QLocalServer::GroupAccessOption);
#ifdef Q_OS_LINUX
        socket_key += QString("%1_").arg(getgid());
#endif
        break;
    case WorldScope:
        _d_singleServer->setSocketOptions(QLocalServer::WorldAccessOption);
        break;
    default:
        _d_singleServer->setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_LINUX
        socket_key += QString("%1_").arg(getuid());
#endif
        break;
    }

    socket_key += key;

#ifdef Q_OS_LINUX
    if (!DInstanceGuard::guard(socket_key)) {
        return false;
    }
    DInstanceGuard::DCriticalHolder holder;
#endif

    // 通知别的实例
    QLocalSocket socket;
    socket.connectToServer(socket_key);

    // 等待到有效数据时认为server实例有效
    if (socket.waitForConnected(DGuiApplicationHelperPrivate::waitTime) && socket.waitForReadyRead(DGuiApplicationHelperPrivate::waitTime)) {
        // 读取数据
        qint8 version;
        qint64 pid;
        QStringList arguments;

        QDataStream ds(&socket);
        ds >> version >> pid >> arguments;
        qInfo() << "Process is started: pid=" << pid << "arguments=" << arguments;

        // 把自己的信息告诉第一个实例
        ds << _d_singleServerVersion << qApp->applicationPid() << qApp->arguments();
        socket.flush();

        return false;
    }

    if (!_d_singleServer->listen(socket_key)) {
        return false;
    }

    if (new_server) {
        QObject::connect(_d_singleServer, &QLocalServer::newConnection, qApp, [] {
            QLocalSocket *instance = _d_singleServer->nextPendingConnection();
            // 先发送数据告诉新的实例自己收到了它的请求
            QDataStream ds(instance);
            ds << _d_singleServerVersion // 协议版本
               << qApp->applicationPid() // 进程id
               << qApp->arguments(); // 启动时的参数

            QObject::connect(instance, &QLocalSocket::readyRead, qApp, [instance] {
                // 读取数据
                QDataStream ds(instance);

                qint8 version;
                qint64 pid;
                QStringList arguments;

                ds >> version >> pid >> arguments;
                instance->close();

                qInfo() << "New instance: pid=" << pid << "arguments=" << arguments;

                // 通知新进程的信息
                if (_globalHelper.exists())
                    Q_EMIT _globalHelper->helper->newProcessInstance(pid, arguments);
            });

            instance->flush(); //发送数据给新的实例
        });
    }

    return true;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setSingelInstanceInterval设置从QLocalServer获取消息的等待时间，
 * \~chinese \brief 用于在重新创建DGuiApplicationHelper单例时，检测DGuiApplicationHelper单例是否存在且有响应
 * \~chinese \param interval等待时间，如 interval 为 -1 则没有超时一直等待，默认和 QLocalSocket 一致 3000ms
 * \~chinese \note 需要在 DGuiApplicationHelper::setSingleInstance 之前调用否则无效。
 */
void DGuiApplicationHelper::setSingleInstanceInterval(int interval)
{
    Q_ASSERT_X(!_d_singleServer->isListening(), "DGuiApplicationHelper::setSingleInstanceInterval","Must call before setSingleInstance");
    DGuiApplicationHelperPrivate::waitTime = interval;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setSingelInstanceInterval设置从QLocalServer获取消息的等待时间
 * \~chinese \param interval等待时间， typo 请使用 DGuiApplicationHelper::setSingleInstanceInterval
 */
void DGuiApplicationHelper::setSingelInstanceInterval(int interval)
{
    DGuiApplicationHelperPrivate::waitTime = interval;
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setThemeType设置主题类型
 * \~chinese \param themeType主题类型的枚举值
 */
void DGuiApplicationHelper::setThemeType(DGuiApplicationHelper::ColorType themeType)
{
    D_D(DGuiApplicationHelper);

    if (d->themeType == themeType)
        return;

    d->themeType = themeType;
    Q_EMIT themeTypeChanged(themeType);
}

/*!
 * \~chinese \brief DGuiApplicationHelper::setPaletteType设置调色板类型
 * \~chinese \param paletteType主题类型的枚举值
 */
void DGuiApplicationHelper::setPaletteType(DGuiApplicationHelper::ColorType paletteType)
{
    D_D(DGuiApplicationHelper);

    if (d->paletteType == paletteType)
        return;

    d->paletteType = paletteType;
    d->notifyAppThemeChanged(qGuiApp, true);

    Q_EMIT paletteTypeChanged(paletteType);
}

DGUI_END_NAMESPACE

#include "moc_dguiapplicationhelper.cpp"
