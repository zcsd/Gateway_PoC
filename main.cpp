#include "hmiauthorization.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    HmiAuthorization w;
    w.show();

    return a.exec();
}
