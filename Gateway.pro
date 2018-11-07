#-------------------------------------------------
#
# Project created by QtCreator 2018-11-07T13:56:06
#
#-------------------------------------------------

QT       += core gui network opcua mqtt

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = Gateway
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++11

# libcurl, to get token from http
LIBS += -lcurl

# RFID reader, using relative path
INCLUDEPATH += ./lib/rfid_s8
LIBS += -L/lib -ls8

SOURCES += \
        main.cpp \
        gateway.cpp \
    mqttclient.cpp \
    rfidtool.cpp

HEADERS += \
        gateway.h \
    mqttclient.h \
    rfidtool.h

FORMS += \
        gateway.ui

