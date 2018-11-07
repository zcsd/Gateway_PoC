#include "gateway.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    Gateway w;
    w.show();

    return a.exec();
}
