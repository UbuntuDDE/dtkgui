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

#include "dthumbnailprovider.h"
#include <DObjectPrivate>

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QImageReader>
#include <QQueue>
#include <QMimeType>
#include <QMimeDatabase>
#include <QReadWriteLock>
#include <QWaitCondition>
#include <QPainter>
#include <QUrl>
#include <QDebug>

#include <DStandardPaths>

DGUI_BEGIN_NAMESPACE

#define FORMAT ".png"
#define THUMBNAIL_PATH \
    DCORE_NAMESPACE::DStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails"
#define THUMBNAIL_FAIL_PATH THUMBNAIL_PATH"/fail"
#define THUMBNAIL_LARGE_PATH THUMBNAIL_PATH"/large"
#define THUMBNAIL_NORMAL_PATH THUMBNAIL_PATH"/normal"
#define THUMBNAIL_SMALL_PATH THUMBNAIL_PATH"/small"

inline QByteArray dataToMd5Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

class DThumbnailProviderPrivate : public DTK_CORE_NAMESPACE::DObjectPrivate
{
public:
    explicit DThumbnailProviderPrivate(DThumbnailProvider *qq);

    void init();

    QString sizeToFilePath(DThumbnailProvider::Size size) const;

    QString errorString;
    // MAX
    qint64 defaultSizeLimit = INT64_MAX;
    QHash<QMimeType, qint64> sizeLimitHash;
    QMimeDatabase mimeDatabase;

    static QSet<QString> hasThumbnailMimeHash;

    struct ProduceInfo
    {
        QFileInfo fileInfo;
        DThumbnailProvider::Size size;
        DThumbnailProvider::CallBack callback;
    };

    QQueue<ProduceInfo> produceQueue;
    QSet<QPair<QString, DThumbnailProvider::Size>> discardedProduceInfos;

    bool running = true;

    QWaitCondition waitCondition;
    QReadWriteLock dataReadWriteLock;

    D_DECLARE_PUBLIC(DThumbnailProvider)
};

QSet<QString> DThumbnailProviderPrivate::hasThumbnailMimeHash;

DThumbnailProviderPrivate::DThumbnailProviderPrivate(DThumbnailProvider *qq)
    : DObjectPrivate(qq)
{

}

void DThumbnailProviderPrivate::init()
{

}

QString DThumbnailProviderPrivate::sizeToFilePath(DThumbnailProvider::Size size) const
{
    switch (size)
    {
    case DThumbnailProvider::Small:
        return THUMBNAIL_SMALL_PATH;
    case DThumbnailProvider::Normal:
        return THUMBNAIL_NORMAL_PATH;
    case DThumbnailProvider::Large:
        return THUMBNAIL_LARGE_PATH;
    }

    return QString();
}

class DFileThumbnailProviderPrivate : public DThumbnailProvider {};
Q_GLOBAL_STATIC(DFileThumbnailProviderPrivate, ftpGlobal)

DThumbnailProvider *DThumbnailProvider::instance()
{
    return ftpGlobal;
}

/*!
 * \~chinese \brief DThumbnailProvider::hasThumbnail缩略图是否存在
 * \~chinese \param info文件信息
 * \~chinese \return true存在　false不存在
 */
bool DThumbnailProvider::hasThumbnail(const QFileInfo &info) const
{
    Q_D(const DThumbnailProvider);

    if (!info.isReadable() || !info.isFile())
    {
        return false;
    }

    qint64 fileSize = info.size();

    if (fileSize <= 0)
    {
        return false;
    }

    const QMimeType &mime = d->mimeDatabase.mimeTypeForFile(info);

    if (fileSize > sizeLimit(mime))
    {
        return false;
    }

    return hasThumbnail(mime);
}

bool DThumbnailProvider::hasThumbnail(const QMimeType &mimeType) const
{
    const QString &mime = mimeType.name();

    if (DThumbnailProviderPrivate::hasThumbnailMimeHash.isEmpty())
    {
        const QList<QByteArray> &mimeTypes = QImageReader::supportedMimeTypes();

        if (mimeTypes.isEmpty())
        {
            DThumbnailProviderPrivate::hasThumbnailMimeHash.insert("");

            return false;
        }

        DThumbnailProviderPrivate::hasThumbnailMimeHash.reserve(mimeTypes.size());

        for (const QByteArray &t : mimeTypes)
        {
            DThumbnailProviderPrivate::hasThumbnailMimeHash.insert(QString::fromLocal8Bit(t));
        }
    }

    return DThumbnailProviderPrivate::hasThumbnailMimeHash.contains(mime);
}

/*!
 * \~chinese \brief DThumbnailProvider::thumbnailFilePath返回文件缩略图文件路径
 * \~chinese \param info文件信息
 * \~chinese \param size 图片大小
 * \~chinese \return 路径信息
 */
QString DThumbnailProvider::thumbnailFilePath(const QFileInfo &info, Size size) const
{
    Q_D(const DThumbnailProvider);

    const QString &absolutePath = info.absolutePath();
    const QString &absoluteFilePath = info.absoluteFilePath();

    if (absolutePath == d->sizeToFilePath(Small)
            || absolutePath == d->sizeToFilePath(Normal)
            || absolutePath == d->sizeToFilePath(Large)
            || absolutePath == THUMBNAIL_FAIL_PATH)
    {
        return absoluteFilePath;
    }

    const QString thumbnailName = dataToMd5Hex(QUrl::fromLocalFile(absoluteFilePath).toString(QUrl::FullyEncoded).toLocal8Bit()) + FORMAT;
    QString thumbnail = d->sizeToFilePath(size) + QDir::separator() + thumbnailName;

    if (!QFile::exists(thumbnail))
    {
        return QString();
    }

    QImage image(thumbnail);

    if (image.text(QT_STRINGIFY(Thumb::MTime)).toInt() != (int)info.lastModified().toTime_t())
    {
        QFile::remove(thumbnail);

        Q_EMIT thumbnailChanged(absoluteFilePath, QString());

        return QString();
    }

    return thumbnail;
}

/*!
 * \~chinese \brief DThumbnailProvider::createThumbnail创建缩略图
 * \~chinese \param info 文件信息
 * \~chinese \param size 图片大小
 * \~chinese \return 成功返回绝对路径信息，失败则返回空
 */
QString DThumbnailProvider::createThumbnail(const QFileInfo &info, DThumbnailProvider::Size size)
{
    Q_D(DThumbnailProvider);

    d->errorString.clear();

    const QString &absolutePath = info.absolutePath();
    const QString &absoluteFilePath = info.absoluteFilePath();

    if (absolutePath == d->sizeToFilePath(Small)
            || absolutePath == d->sizeToFilePath(Normal)
            || absolutePath == d->sizeToFilePath(Large)
            || absolutePath == THUMBNAIL_FAIL_PATH)
    {
        return absoluteFilePath;
    }

    if (!hasThumbnail(info))
    {
        d->errorString = QStringLiteral("This file has not support thumbnail: ") + absoluteFilePath;

        //!Warnning: Do not store thumbnails to the fail path
        return QString();
    }

    const QString fileUrl = QUrl::fromLocalFile(absoluteFilePath).toString(QUrl::FullyEncoded);
    const QString thumbnailName = dataToMd5Hex(fileUrl.toLocal8Bit()) + FORMAT;

    // the file is in fail path
    QString thumbnail = THUMBNAIL_FAIL_PATH + QDir::separator() + thumbnailName;

    if (QFile::exists(thumbnail))
    {
        QImage image(thumbnail);

        if (image.text(QT_STRINGIFY(Thumb::MTime)).toInt() != (int)info.lastModified().toTime_t())
        {
            QFile::remove(thumbnail);
        }
        else
        {
            return QString();
        }
    }// end

    QScopedPointer<QImage> image(new QImage(QSize(size, size), QImage::Format_ARGB32_Premultiplied));
    QImageReader reader(absoluteFilePath);

    if (!reader.canRead())
    {
        reader.setFormat(d->mimeDatabase.mimeTypeForFile(info).name().toLocal8Bit());

        if (!reader.canRead())
        {
            d->errorString = reader.errorString();
        }
    }

    if (d->errorString.isEmpty())
    {
        const QSize &imageSize = reader.size();

        if (imageSize.isValid())
        {
            if (imageSize.width() >= size || imageSize.height() >= size)
            {
                reader.setScaledSize(reader.size().scaled(size, size, Qt::KeepAspectRatio));
            }

            if (!reader.read(image.data()))
            {
                d->errorString = reader.errorString();
            }
        }
        else
        {
            d->errorString = "Fail to read image file attribute data:" + info.absoluteFilePath();
        }
    }

    // successful
    if (d->errorString.isEmpty())
    {
        thumbnail = d->sizeToFilePath(size) + QDir::separator() + thumbnailName;
    }
    else
    {
        //fail
        image.reset(new QImage(1, 1, QImage::Format_Mono));
    }

    image->setText(QT_STRINGIFY(Thumb::URL), fileUrl);
    image->setText(QT_STRINGIFY(Thumb::MTime), QString::number(info.lastModified().toTime_t()));

    // create path
    QFileInfo(thumbnail).absoluteDir().mkpath(".");

    if (!image->save(thumbnail, Q_NULLPTR, 80))
    {
        d->errorString = QStringLiteral("Can not save image to ") + thumbnail;
    }

    if (d->errorString.isEmpty())
    {
        Q_EMIT createThumbnailFinished(absoluteFilePath, thumbnail);
        Q_EMIT thumbnailChanged(absoluteFilePath, thumbnail);

        return thumbnail;
    }

    // fail
    Q_EMIT createThumbnailFailed(absoluteFilePath);

    return QString();
}

void DThumbnailProvider::appendToProduceQueue(const QFileInfo &info, DThumbnailProvider::Size size, DThumbnailProvider::CallBack callback)
{
    DThumbnailProviderPrivate::ProduceInfo produceInfo;

    produceInfo.fileInfo = info;
    produceInfo.size = size;
    produceInfo.callback = callback;

    Q_D(DThumbnailProvider);

    if (isRunning())
    {
        QWriteLocker locker(&d->dataReadWriteLock);
        d->produceQueue.append(std::move(produceInfo));
        locker.unlock();
        d->waitCondition.wakeAll();
    }
    else
    {
        d->produceQueue.append(std::move(produceInfo));
        start();
    }
}

/*!
 * \~chinese \brief DThumbnailProvider::removeInProduceQueue将缩略图从列表中删除
 * \~chinese \param info缩略图文件
 * \~chinese \param size缩略图大小
 */
void DThumbnailProvider::removeInProduceQueue(const QFileInfo &info, DThumbnailProvider::Size size)
{
    Q_D(DThumbnailProvider);

    if (isRunning())
    {
        QWriteLocker locker(&d->dataReadWriteLock);
        Q_UNUSED(locker)
    }

    d->discardedProduceInfos.insert(qMakePair(info.absoluteFilePath(), size));
}

/*!
 * \~chinese \brief DThumbnailProvider::errorString返回错误信息
 * \~chinese \return 错误信息
 */
QString DThumbnailProvider::errorString() const
{
    Q_D(const DThumbnailProvider);

    return d->errorString;
}

/*!
 * \~chinese \brief DThumbnailProvider::defaultSizeLimit返回缩略图默认大小
 * \~chinese \return 默认的大小
 */
qint64 DThumbnailProvider::defaultSizeLimit() const
{
    Q_D(const DThumbnailProvider);

    return d->defaultSizeLimit;
}

/*!
 * \~chinese \brief DThumbnailProvider::setDefaultSizeLimit设置缩略图的默认大小
 * \~chinese \param size 大小
 */
void DThumbnailProvider::setDefaultSizeLimit(qint64 size)
{
    Q_D(DThumbnailProvider);

    d->defaultSizeLimit = size;
}

/*!
 * \~chinese \brief DThumbnailProvider::sizeLimit　返回文件大小
 * \~chinese \param mimeType 由MIME类型字符串表示的文件或数据类型
 * \~chinese \return
 */
qint64 DThumbnailProvider::sizeLimit(const QMimeType &mimeType) const
{
    Q_D(const DThumbnailProvider);

    return d->sizeLimitHash.value(mimeType, d->defaultSizeLimit);
}

/*!
 * \~chinese \brief DThumbnailProvider::setSizeLimit 设置文件的大小
 * \~chinese \param mimeType 由MIME类型字符串表示的文件或数据类型
 * \~chinese \param size 范围
 */
void DThumbnailProvider::setSizeLimit(const QMimeType &mimeType, qint64 size)
{
    Q_D(DThumbnailProvider);

    d->sizeLimitHash[mimeType] = size;
}

/*!
 * \~chinese \class DThumbnailProvider
 * \~chinese \brief 缩略图生成类
 * \~chinese　\note 缩略图创建失败
 * \~chinese \li 该文件格式未知，无法由程序加载。
 * \~chinese \li 文件格式是已知的，但是文件已被损坏，因此无法读取。
 * \~chinese \li 由于文件很大，缩略图的生成将花费很长时间
 */

DThumbnailProvider::DThumbnailProvider(QObject *parent)
    : QThread(parent)
    , DObject(*new DThumbnailProviderPrivate(this))
{
    d_func()->init();
}

DThumbnailProvider::~DThumbnailProvider()
{
    Q_D(DThumbnailProvider);

    d->running = false;
    d->waitCondition.wakeAll();
    wait();
}

void DThumbnailProvider::run()
{
    Q_D(DThumbnailProvider);

    Q_FOREVER
    {
        QWriteLocker locker(&d->dataReadWriteLock);

        if (d->produceQueue.isEmpty())
        {
            d->waitCondition.wait(&d->dataReadWriteLock);
        }

        if (!d->running)
        {
            return;
        }

        const DThumbnailProviderPrivate::ProduceInfo &task = d->produceQueue.dequeue();
        const QPair<QString, DThumbnailProvider::Size> &tmpKey = qMakePair(task.fileInfo.absoluteFilePath(), task.size);

        if (d->discardedProduceInfos.contains(tmpKey))
        {
            d->discardedProduceInfos.remove(tmpKey);
            locker.unlock();
            continue;
        }

        locker.unlock();

        const QString &thumbnail = createThumbnail(task.fileInfo, task.size);

        if (task.callback)
        {
            task.callback(thumbnail);
        }
    }
}

DGUI_END_NAMESPACE
