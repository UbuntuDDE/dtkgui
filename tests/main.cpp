#include <gtest/gtest.h>

#include <QGuiApplication>
#include <gtest/gtest.h>

int main(int argc, char *argv[])
{
    // gerrit编译时没有显示器，需要指定环境变量
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
