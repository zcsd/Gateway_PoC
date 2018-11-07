#include "gateway.h"
#include "ui_gateway.h"

Gateway::Gateway(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::Gateway)
{
    ui->setupUi(this);

    connect(ui->listWidget->model(), SIGNAL(rowsInserted(QModelIndex,int,int)),
            ui->listWidget, SLOT(scrollToBottom()));
    initSetup();
}

Gateway::~Gateway()
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

void Gateway::initSetup()
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

    connect(this, SIGNAL(readyToSendJobRequest()), this, SLOT(prepareToSendJobRequest()));

    opcuaProvider = new QOpcUaProvider(this);
    httpRest = new QNetworkAccessManager(this);

    rfidTool = new RFIDTool(this);
    connect(rfidTool, SIGNAL(sendDeviceInfo(QString)), this, SLOT(receiveRFIDDeviceInfo(QString)));
    connect(rfidTool, SIGNAL(sendReadInfo(bool, QString, QString)), this, SLOT(receiveRFIDReadInfo(bool, QString, QString)));
}

void Gateway::connectToOPCUAServer()
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

    connect(opcuaClient, &QOpcUaClient::connected, this, &Gateway::opcuaConnected);
    connect(opcuaClient, &QOpcUaClient::disconnected, this, &Gateway::opcuaDisconnected);
    connect(opcuaClient, &QOpcUaClient::errorChanged, this, &Gateway::opcuaError);
    connect(opcuaClient, &QOpcUaClient::stateChanged, this, &Gateway::opcuaState);

    opcuaClient->connectToEndpoint(ui->lineEditOpcUrl->text()); // connect action
}

void Gateway::diconnectToOPCUAServer()
{
    if (isOpcUaConnected)
    {
        opcuaClient->disconnectFromEndpoint();
    }
}

void Gateway::getHMILoginAuth(QString username, QString password, QString service)
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

void Gateway::startRFID()
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

void Gateway::closeRFID()
{
    rfidThread->quit();
    rfidThread->wait();

    rfidTool->closeDevice();
    isRFIDStart = false;
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                            + "Disconnect to RFID Scanner.");
}

void Gateway::connectMqtt()
{
    mqttClient = new MqttClient(this);
    //connect(mqttClient, SIGNAL(sendConState(int)), this, SLOT(receiveMqttConState(int)));
    connect(mqttClient, &MqttClient::sendConState, this, [this](int state)
    {
        if (state == 1)
        {
            isMqttConnected = true;
            // get job list information w.r.t rfid card
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

void Gateway::disconnectMqtt()
{
    if (isMqttConnected)
    {
        mqttClient->disconnect();
    }
    delete mqttClient;
}

void Gateway::opcuaConnected()
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

    jobIDNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_ID"); //string
    jobNameNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_ProcessName"); //string
    materialCodeNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_MaterialCode"); // string
    jobRecipeNameNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_RecipeName"); // string
    jobPlanQtyNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_PlanQty");  // int32
    jobPlanStartTimeNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_PlanStartTime"); //string
    jobPlanEndTimeNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_PlanEndTime");  // string

    jobApproveNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job_approve");  // int16
    visionResultR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.vision.RESULT"); // uint16
    goodPartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.good_parts"); // int32
    rejectSizePartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.rejectSize_parts"); //int32
    rejectColorPartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.rejectColor_parts"); // int32
    totalPartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.total_parts"); //int32
    conveyorSpeedNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.parameters.conveyor_Speed"); //int16

    ///////////////////////////////////////////////Job Request, Vision Status, Power Status////////////////////////////////////////////
    powerStatusNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.power_status"); // uint16
    powerStatusNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(powerStatusNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read powerStatus node:" << value.toInt();
        ui->labelPowerStatus->setNum(value.toInt());
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Power Status in OPCUA server updated.");
        if (value.toInt() == 1)
        {
            isPowerReady = true;
            emit readyToSendJobRequest();
        }
        else
        {
            isPowerReady = false;
        }
    });

    visionStatusNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.vision.VISION_STATUS"); // uint16
    visionStatusNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(visionStatusNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read visionStatus node:" << value.toInt();
        ui->labelVisionStatus->setNum(value.toInt());
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Vision Status in OPCUA server updated.");
        if (value.toInt() == 1)
        {
            isVisionReady = true;
            emit readyToSendJobRequest();
        }
        else
        {
            isVisionReady = false;
        }
    });

    jobRequestNodeRW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job_request"); // int16
    jobRequestNodeRW->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(jobRequestNodeRW, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read jobRequest node:" << value.toInt();
        ui->labelJobRequest->setNum(value.toInt());
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Job Request in OPCUA server updated.");
        if (value.toInt() == 1)
        {
            isJobRequest = true;
            emit readyToSendJobRequest();
        }
        else
        {
            isJobRequest = false;
        }
    });
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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
    connect(hmiLoginRequestNodeRW, &QOpcUaNode::enableMonitoringFinished, this, &Gateway::enableMonitoringFinished);
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

void Gateway::opcuaDisconnected()
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

void Gateway::opcuaError(QOpcUaClient::ClientError error)
{
    qDebug() << "OPCUA Client Error:" << error;
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                              + "OPCUA Client Error: " + error);
}

void Gateway::opcuaState(QOpcUaClient::ClientState state)
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

void Gateway::enableMonitoringFinished(QOpcUa::NodeAttribute attr, QOpcUa::UaStatusCode status)
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

void Gateway::prepareToGetHMIAuth()
{
    if (requestToLogin && hmiUsernameReady && hmiPasswordReady)
    {
        getHMILoginAuth(hmiUsername, hmiPassword, "factory");
    }
}

void Gateway::writeAuthResultToOpcua(int isAuth, QString displayUserName, int accessLevel)
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

void Gateway::finishWrittenToOpcUa()
{
    if (authRightWritten && displayUsernameWritten && accessLevelWritten)
    {
        if (hmiLoginRequestNodeRW)
        {
            hmiLoginRequestNodeRW->writeAttribute(QOpcUa::NodeAttribute::Value, 0, QOpcUa::Int16);
        }
    }
}

void Gateway::receiveRFIDDeviceInfo(QString port)
{
    ui->labelRFIDPort->setText(port);
}

void Gateway::receiveRFIDReadInfo(bool isValid, QString card, QString data)
{
    if (isValid)
    {
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + card + ": " +  data.split(":")[2]);
        ui->labelRFIDRead->setText(card);
        cardID = card;
        isMaterialReady = true;
        emit readyToSendJobRequest();
    }
    else
    {
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + card);
    }
}

void Gateway::receiveMqttSubMsg(QString topic, QString msg)
{
    // qDebug() << topic << msg;
    // "v1/devices/me/rpc/request/+"
    // {"method":"alarmTrigger","params":{"JobID":12345,"JobName":"Win"}}
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                      + "Msg from mqtt:" + msg);

    if (topic.contains("v1/devices/me/rpc/request", Qt::CaseInsensitive))
    {
        const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        const QJsonObject obj = doc.object();

        //QString method = obj.value(QLatin1String("method")).toString();

        int jobID = obj.value(QLatin1String("params")).toObject().value(QLatin1String("JobID")).toInt();
        QString jobName = obj.value(QLatin1String("params")).toObject().value(QLatin1String("JobName")).toString();
        ui->labelJobID->setNum(jobID);
        ui->labelJobProcess->setText(jobName);
        qDebug() << jobID << jobName;

        jobIDNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, QString::number(jobID), QOpcUa::String);
        jobNameNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobName, QOpcUa::String);
        materialCodeNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, "test_mc", QOpcUa::String);
        jobRecipeNameNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, "test_rn", QOpcUa::String);
        jobPlanQtyNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, 5, QOpcUa::Int32);
        jobPlanStartTimeNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, "2018-11-02 15:00", QOpcUa::String);
        jobPlanEndTimeNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, "2018-11-02 18:30", QOpcUa::String);
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Write job information to OPCUA server");
    }
    else
    {

    }

}

void Gateway::prepareToSendJobRequest()
{
    if (isJobRequest && isVisionReady && isPowerReady && isMaterialReady)
    {
        // it should publish card infor, but we use hardcode for testing.
        QString toSent = QString("{'t': 30, 'tagID': %1}").arg(cardID);
        mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        isMaterialReady = false;
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Check status ok, sent job request to Thingsboard.");
    }
}

void Gateway::on_pushButtonStart_clicked()
{
    if (!isOpcUaConnected)
    {
        ui->labelID->clear();
        ui->labelPassword->clear();
        ui->labelTimeLogin->clear();
        ui->labelApprove->clear();
        ui->labelName->clear();
        ui->labelAccessLevel->clear();
        ui->labelPowerStatus->clear();
        ui->labelVisionStatus->clear();
        connectToOPCUAServer();
    }

    if (!isMqttConnected)
    {
        ui->labelJobID->clear();
        ui->labelJobProcess->clear();
        connectMqtt();
    }
    ui->labelRFIDRead->clear();
    startRFID();
}

void Gateway::on_pushButtonStop_clicked()
{
    if (isOpcUaConnected)
        diconnectToOPCUAServer();
    if (isMqttConnected)
        disconnectMqtt();
    closeRFID();
    isJobRequest = false;
    isPowerReady = false;
    isVisionReady = false;
    isMaterialReady = false;
    ui->labelRFIDPort->clear();
    ui->labelRFIDRead->clear();
    ui->labelID->clear();
    ui->labelPassword->clear();
    ui->labelTimeLogin->clear();
    ui->labelApprove->clear();
    ui->labelName->clear();
    ui->labelAccessLevel->clear();
    ui->labelPowerStatus->clear();
    ui->labelVisionStatus->clear();
    ui->labelJobID->clear();
    ui->labelJobProcess->clear();
}
