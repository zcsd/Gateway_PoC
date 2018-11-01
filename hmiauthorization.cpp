#include "hmiauthorization.h"
#include "ui_hmiauthorization.h"

HmiAuthorization::HmiAuthorization(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::HmiAuthorization)
{
    ui->setupUi(this);

    connect(ui->listWidget->model(), SIGNAL(rowsInserted(QModelIndex,int,int)),
            ui->listWidget, SLOT(scrollToBottom()));
    initSetup();
}

HmiAuthorization::~HmiAuthorization()
{
    if (isOpcUaConnected)
    {
        diconnectToOPCUAServer();
    }
    delete ui;
    delete opcuaProvider;
    delete httpRest;
    delete rfidTool;
    delete rfidTimer;
    delete rfidThread;
}

void HmiAuthorization::initSetup()
{
    if (!isOpcUaConnected)
    {
        ui->pushButtonStop->setDisabled(true);
        ui->pushButtonStart->setEnabled(true);
        ui->pushButtonStart->setStyleSheet("background-color: rgb(225, 225, 225);"); // gray
    }
    else
    {
        ui->pushButtonStart->setDisabled(true);
        ui->pushButtonStop->setEnabled(true);
        ui->pushButtonStart->setStyleSheet("background-color: rgb(100, 255, 100);"); // green
    }

    connect(this, SIGNAL(readyToGetHMIAuth()), this, SLOT(prepareToGetHMIAuth()));
    connect(this, SIGNAL(sendAuthResult(int, QString, int)), this, SLOT(writeAuthResultToOpcua(int, QString, int)));
    connect(this, SIGNAL(authResultWrittenToOpcUa()), this, SLOT(finishWrittenToOpcUa()));

    opcuaProvider = new QOpcUaProvider(this);
    httpRest = new QNetworkAccessManager(this);

    rfidTool = new RFIDTool(this);
    connect(rfidTool, SIGNAL(sendDeviceInfo(QString)), this, SLOT(receiveRFIDDeviceInfo(QString)));
    connect(rfidTool, SIGNAL(sendReadInfo(bool, QString, QString)), this, SLOT(receiveRFIDReadInfo(bool, QString, QString)));

}

void HmiAuthorization::connectToOPCUAServer()
{
    //const static QUrl opcuaServer(QLatin1String("opc.tcp://172.19.80.34:4840"));
    // default plugin is open62541
    qDebug() << "Available OPCUA plugins:" << opcuaProvider->availableBackends();
    opcuaClient = opcuaProvider->createClient(opcuaProvider->availableBackends()[0]);

    if (!opcuaClient)
    {
        qDebug() << "Fail to create OPCUA client.";
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                  + "Fail to create OPCUA client.");
        return;
    }

    connect(opcuaClient, &QOpcUaClient::connected, this, &HmiAuthorization::opcuaConnected);
    connect(opcuaClient, &QOpcUaClient::disconnected, this, &HmiAuthorization::opcuaDisconnected);
    connect(opcuaClient, &QOpcUaClient::errorChanged, this, &HmiAuthorization::opcuaError);
    connect(opcuaClient, &QOpcUaClient::stateChanged, this, &HmiAuthorization::opcuaState);

    opcuaClient->connectToEndpoint(ui->lineEditOpcUrl->text()); // connect action
}

void HmiAuthorization::diconnectToOPCUAServer()
{
    if (isOpcUaConnected)
    {
        opcuaClient->disconnectFromEndpoint();
    }
}

void HmiAuthorization::getHMILoginAuth(QString username, QString password, QString service)
{
    qDebug() << "Sending http requset to get autho information for PLC HMI...";
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                              + "Sending HMI login info to autho server...");
    //use http rest api to get auth right, with json
    //const static QUrl authServer(QLatin1String("http://sat-mes/server/auth/authenticate"));
    const static QUrl authServer(ui->lineEditAuthoUrl->text());

    QNetworkRequest request(authServer);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject obj
    {
        {QStringLiteral("username"), username},
        {QStringLiteral("password"), password},
        {QStringLiteral("service"), service},
    };

    QNetworkReply *reply = httpRest->post(request, QJsonDocument(obj).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject obj = doc.object();

        int authCode = obj.value(QLatin1String("result")).toInt();
        QString displayUserName = obj.value(QLatin1String("displayName")).toString();
        QString accessLevel = obj.value(QLatin1String("rights")).toObject().value(QLatin1String("mespoc")).toObject().value(QLatin1String("mespoc")).toObject().value(QLatin1String("User")).toString();

        qDebug() << "Get autho result from autho server:" << authCode << displayUserName << accessLevel;
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                  + "Get autho information from autho server.");

        if (authCode == 1)
        {
            authCode = 8; // int 8 for approve in PLC
            emit sendAuthResult(authCode, displayUserName, accessLevel.toInt());
            ui->labelApprove->setNum(authCode);
            ui->labelName->setText(displayUserName);
            ui->labelAccessLevel->setText(accessLevel);
        }
        else
        {
            authCode = 7; // int 7 for rejected in PLC
            emit sendAuthResult(authCode, "NA", 0);
            ui->labelApprove->setNum(authCode);
            ui->labelName->setText("NA");
            ui->labelAccessLevel->setText("NA");
        }


    });
}

void HmiAuthorization::startRFID()
{
    if (rfidTool->initDevice())
    {
        isRFIDStart = true;
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Connect to RFID Scanner.");
        rfidThread = new QThread(this);
        rfidThread->start();

        rfidTimer = new QTimer();
        rfidTimer->setInterval(500);

        connect(rfidTimer, SIGNAL(timeout()), rfidTool, SLOT(icode2()), Qt::DirectConnection);
        connect(rfidThread, SIGNAL(finished()), rfidTimer, SLOT(stop()));

        rfidTimer->start();
        //run timer work(loop reading RFID tag) in thread
        rfidTimer->moveToThread(rfidThread);
    }
}

void HmiAuthorization::closeRFID()
{
    rfidThread->quit();
    rfidThread->wait();

    rfidTool->closeDevice();
    isRFIDStart = false;
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                            + "Disconnect to RFID Scanner.");
}

void HmiAuthorization::connectMqtt()
{
    mqttClient = new MqttClient(this);
    //connect(mqttClient, SIGNAL(sendConState(int)), this, SLOT(receiveMqttConState(int)));
    connect(mqttClient, &MqttClient::sendConState, this, [this](int state)
    {
        if (state == 1)
        {
            isMqttConnected = true;
            mqttClient->subscribe("v1/devices/me/rpc/request/+", 0);
            connect(mqttClient, SIGNAL(sendSubMsg(QString, QString)), this, SLOT(receiveMqttSubMsg(QString, QString)));
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Connected to Mqtt broker.");
        }
        else
        {
            isMqttConnected = false;
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Disconnected to Mqtt broker.");
        }
    });

    QString mqttUsername = "tenant@thingsboard.org", mqttPassword = "tenant";
    QString mqttDeviceID = "624facf0-cb6e-11e8-8891-05a8a3fcf36e";
    if (mqttClient->isConnected())
    {
        isMqttConnected = true;
    }
    else
    {
        mqttClient->connectToBroker(ui->lineEditMqttUrl->text().split(":")[0], ui->lineEditMqttUrl->text().split(":")[1],
                                    mqttUsername, mqttPassword, mqttDeviceID, "8080");
        mqttClient->keepAlive(25);
    }
}

void HmiAuthorization::disconnectMqtt()
{
    if (isMqttConnected)
    {
        mqttClient->disconnect();
    }
    delete mqttClient;
}

void HmiAuthorization::mqttPublishRFID()
{

}

void HmiAuthorization::opcuaConnected()
{
    isOpcUaConnected = true;
    ui->pushButtonStart->setDisabled(true);
    ui->pushButtonStop->setEnabled(true);
    ui->pushButtonStart->setStyleSheet("background-color: rgb(100, 255, 100);"); // green

    authRightNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.autho_approve"); //int16
    connect(authRightNodeW, &QOpcUaNode::attributeWritten, this, [this](QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status)
    {
        if (attr == QOpcUa::NodeAttribute::Value && status == QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Write autho_approve to opcua server successfully.";
            authRightWritten = true;
            emit authResultWrittenToOpcUa();
        }
        else if (attr == QOpcUa::NodeAttribute::Value && status != QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Failed to write autho_approve to opcua server.";
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Failed to write autho_approve to opcua server.");
            authRightWritten = false;
        }
    });

    displayUsernameNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.autho_name"); //string
    connect(displayUsernameNodeW, &QOpcUaNode::attributeWritten, this, [this](QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status)
    {
        if (attr == QOpcUa::NodeAttribute::Value && status == QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Write display-username to opcua server successfully.";
            displayUsernameWritten = true;
            emit authResultWrittenToOpcUa();
        }
        else if (attr == QOpcUa::NodeAttribute::Value && status != QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Failed to write display-username to opcua server.";
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Failed to write display-username to opcua server.");
            displayUsernameWritten = false;
        }
    });

    accessLevelNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.autho_accessLevel"); //int16
    connect(accessLevelNodeW, &QOpcUaNode::attributeWritten, this, [this](QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status)
    {
        if (attr == QOpcUa::NodeAttribute::Value && status == QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Write accesslevel to opcua server successfully.";
            accessLevelWritten = true;
            emit authResultWrittenToOpcUa();
        }
        else if (attr == QOpcUa::NodeAttribute::Value && status != QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Failed to write accesslevel to opcua server.";
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Failed to write accesslevel to opcua server.");
            accessLevelWritten = false;
        }
    });

    usernameNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.username"); // string
    connect(usernameNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read username node:" << value.toString().replace(" ", "");
        ui->labelID->setText(value.toString());
        ui->labelTimeLogin->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
        if (value.toString().length() >= 2)
        {
            hmiUsername = value.toString().replace(" ", "");
            hmiUsernameReady = true;
            emit readyToGetHMIAuth();
        }
    });

    passwordNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.password"); // string
    connect(passwordNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read password node:" << value.toString();
        //ui->labelPassword->setText(value.toString());
        ui->labelPassword->setText("********");
        if (value.toString().length() >= 2)
        {
            hmiPassword = value.toString();
            hmiPasswordReady = true;
            emit readyToGetHMIAuth();
        }
    });

    // HMI Login Request Node (read and write)
    hmiLoginRequestNodeRW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.autho_request"); // int16
    connect(hmiLoginRequestNodeRW, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read autho-requset node:" << value.toInt();
        if (value.toInt() == 1)
        {
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Reading HMI login info from OPC UA server...");
            requestToLogin = true;
            usernameNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
            passwordNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
        }
    });
    connect(hmiLoginRequestNodeRW, &QOpcUaNode::enableMonitoringFinished, this, &HmiAuthorization::enableMonitoringFinished);
    hmiLoginRequestNodeRW->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(hmiLoginRequestNodeRW, &QOpcUaNode::attributeWritten, this, [this](QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status)
    {
        if (attr == QOpcUa::NodeAttribute::Value && status == QOpcUa::UaStatusCode::Good)
        {
            qDebug() << "Write(Change) auth_request(reset to 0) to opcua server successfully.";

            usernameNodeR->disableMonitoring(QOpcUa::NodeAttribute::Value);
            passwordNodeR->disableMonitoring(QOpcUa::NodeAttribute::Value);
            requestToLogin = false;
            authRightWritten = false;
            displayUsernameWritten = false;
            accessLevelWritten = false;
            hmiUsernameReady = false;
            hmiPasswordReady = false;
            hmiUsername = "";
            hmiPassword = "";
            qDebug() << "Finish to write autho infor to OPC UA server.";
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Finish to write autho infor to OPC UA server.");
        }
        else if (attr == QOpcUa::NodeAttribute::Value && status != QOpcUa::UaStatusCode::Good)
        {
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Failed to write(change) auth_request to opcua server.");
            qDebug() << "Failed to write(change) auth_request to opcua server.";
        }
    });
}

void HmiAuthorization::opcuaDisconnected()
{
    isOpcUaConnected = false;
    opcuaClient->deleteLater();
    ui->pushButtonStop->setDisabled(true);
    ui->pushButtonStart->setEnabled(true);
    ui->pushButtonStart->setStyleSheet("background-color: rgb(225, 225, 225);"); // gray
    ui->labelID->clear();
    ui->labelPassword->clear();
    ui->labelTimeLogin->clear();
    ui->labelApprove->clear();
    ui->labelName->clear();
    ui->labelAccessLevel->clear();
}

void HmiAuthorization::opcuaError(QOpcUaClient::ClientError error)
{
    qDebug() << "OPCUA Client Error:" << error;
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                              + "OPCUA Client Error: " + error);
}

void HmiAuthorization::opcuaState(QOpcUaClient::ClientState state)
{
    if (state == QOpcUaClient::ClientState::Connected)
    {
        qDebug() << "Successfully connected to OPCUA server.";
        isOpcUaConnected = true;
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                  + "Successfully connected to OPC UA server.");
    }
    else if (state == QOpcUaClient::ClientState::Connecting)
    {
        qDebug() << "Trying to connect OPCUA server now.";
        isOpcUaConnected = false;
    }
    else if (state == QOpcUaClient::ClientState::Disconnected)
    {
        qDebug() << "Disconnected to OPCUA server.";
        isOpcUaConnected = false;
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                  + "Disconnected to OPCUA server.");
    }
}

void HmiAuthorization::enableMonitoringFinished(QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status)
{
    Q_UNUSED(attr);

    if (!sender())
    {
        return;
    }

    if (status == QOpcUa::UaStatusCode::Good)
    {
        qDebug() << "Monitoring successfully enabled for" << qobject_cast<QOpcUaNode *>(sender())->nodeId();
    }
    else
    {
        qDebug() << "Failed to enable monitoring for" << qobject_cast<QOpcUaNode *>(sender())->nodeId();
    }
}

void HmiAuthorization::prepareToGetHMIAuth()
{
    if (requestToLogin && hmiUsernameReady && hmiPasswordReady)
    {
        getHMILoginAuth(hmiUsername, hmiPassword, "factory");
    }
}

void HmiAuthorization::writeAuthResultToOpcua(int isAuth, QString displayUserName, int accessLevel)
{
    if (authRightNodeW)
    {
       authRightNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, isAuth, QOpcUa::Int16);
    }
    if (displayUsernameNodeW)
    {
        displayUsernameNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, displayUserName, QOpcUa::String);
    }
    if (accessLevelNodeW)
    {
        accessLevelNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, accessLevel, QOpcUa::Int16);
    }
}

void HmiAuthorization::finishWrittenToOpcUa()
{
    if (authRightWritten && displayUsernameWritten && accessLevelWritten)
    {
        if (hmiLoginRequestNodeRW)
        {
            hmiLoginRequestNodeRW->writeAttribute(QOpcUa::NodeAttribute::Value, 0, QOpcUa::Int16);
        }
    }
}

void HmiAuthorization::receiveRFIDDeviceInfo(QString port)
{
    ui->labelRFIDPort->setText(port);
}

void HmiAuthorization::receiveRFIDReadInfo(bool isValid, QString card, QString data)
{
    if (isValid)
    {
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + card + ": " +  data.split(":")[2]);
        mqttClient->publish("v1/devices/me/telemetry", "{'t': 29, 'h': 16}", 0);
    }
    else
    {
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + card);
    }
}

void HmiAuthorization::receiveMqttSubMsg(QString topic, QString msg)
{
    qDebug() << topic << msg;
}

void HmiAuthorization::on_pushButtonStart_clicked()
{
    if (!isOpcUaConnected)
    {
        ui->labelID->clear();
        ui->labelPassword->clear();
        ui->labelTimeLogin->clear();
        ui->labelApprove->clear();
        ui->labelName->clear();
        ui->labelAccessLevel->clear();
        connectToOPCUAServer();
    }

    if (!isMqttConnected)
    {
        connectMqtt();
    }

    startRFID();
}

void HmiAuthorization::on_pushButtonStop_clicked()
{
    if (isOpcUaConnected)
        diconnectToOPCUAServer();
    if (isMqttConnected)
        disconnectMqtt();
    closeRFID();
    ui->labelRFIDPort->clear();
}
