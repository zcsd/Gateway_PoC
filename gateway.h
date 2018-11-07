#ifndef GATEWAY_H
#define GATEWAY_H

#include <QMainWindow>
#include <QObject>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QThread>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QOpcUaClient>
#include <QOpcUaNode>
#include <QtOpcUa>

#include "mqttclient.h"
#include "rfidtool.h"

namespace Ui {
class Gateway;
}

class Gateway : public QMainWindow
{
    Q_OBJECT

public:
    explicit Gateway(QWidget *parent = nullptr);
    ~Gateway();

signals:
    void sendAuthResult(int, QString, int);
    void readyToGetHMIAuth();
    void authResultWrittenToOpcUa();

private slots:
    void opcuaConnected();
    void opcuaDisconnected();
    void opcuaError(QOpcUaClient::ClientError error);
    void opcuaState(QOpcUaClient::ClientState state);
    void enableMonitoringFinished(QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status);

    void prepareToGetHMIAuth();
    void writeAuthResultToOpcua(int isAuth, QString displayUserName, int accessLevel);
    void finishWrittenToOpcUa();

    void receiveRFIDDeviceInfo(QString port);
    void receiveRFIDReadInfo(bool isValid, QString card, QString data);

    void receiveMqttSubMsg(QString topic, QString msg);

    void on_pushButtonStart_clicked();
    void on_pushButtonStop_clicked();

private:
    Ui::Gateway *ui;

    RFIDTool *rfidTool;
    QTimer *rfidTimer;
    QThread *rfidThread;
    bool isRFIDStart = false;

    QNetworkAccessManager *httpRest;
    bool requestToLogin = false, hmiUsernameReady = false, hmiPasswordReady = false;
    QString hmiUsername, hmiPassword;

    QOpcUaProvider *opcuaProvider;
    QOpcUaClient *opcuaClient;
    QOpcUaNode *hmiLoginRequestNodeRW;
    QOpcUaNode *usernameNodeR;
    QOpcUaNode *passwordNodeR;
    QOpcUaNode *authRightNodeW;
    QOpcUaNode *displayUsernameNodeW;
    QOpcUaNode *accessLevelNodeW;
    QOpcUaNode *jobIDNodeW, *jobNameNodeW, *materialCodeNodeW, *jobRecipeNameNodeW, *jobPlanQtyNodeW, *jobPlanStartTimeNodeW, *jobPlanEndTimeNodeW;
    QOpcUaNode *powerStatusNodeR, *visionStatusNodeR;

    bool isOpcUaConnected = false;
    bool authRightWritten = false, displayUsernameWritten = false, accessLevelWritten = false;

    MqttClient *mqttClient;
    bool isMqttConnected = false;

    void initSetup();
    void connectToOPCUAServer();
    void diconnectToOPCUAServer();
    void getHMILoginAuth(QString username, QString password, QString service = "factory");
    void startRFID();
    void closeRFID();
    void connectMqtt();
    void disconnectMqtt();

};

#endif // GATEWAY_H
