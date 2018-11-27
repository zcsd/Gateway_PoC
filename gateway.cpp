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
    if (isMqttConnected)
    {
        mqttClient->disconnect();
    }
    if (isRFIDStart)
    {
        closeRFID();
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
    if (!isGatewayReady)
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

    connect(this, SIGNAL(readyToStartGateway()), this, SLOT(prepareToStartGateway()));
    connect(this, SIGNAL(readyToGetHMIAuth()), this, SLOT(prepareToGetHMIAuth()));
    connect(this, SIGNAL(readyToSendJobRequest()), this, SLOT(prepareToSendJobRequest()));
    connect(this, SIGNAL(readyToResetVisionResult()), this, SLOT(prepareToResetVisionResult()));
    connect(this, SIGNAL(sendAuthResult(int, QString, int)), this, SLOT(writeAuthResultToOpcua(int, QString, int)));
    connect(this, SIGNAL(authResultWrittenToOpcUa()), this, SLOT(finishWrittenToOpcUa()));

    opcuaProvider = new QOpcUaProvider(this);
    httpRest = new QNetworkAccessManager(this);

    rfidTool = new RFIDTool(this);
    connect(rfidTool, SIGNAL(sendDeviceInfo(QString)), this, SLOT(receiveRFIDDeviceInfo(QString)));
    connect(rfidTool, SIGNAL(sendReadInfo(bool, QString, QString)), this, SLOT(receiveRFIDReadInfo(bool, QString, QString)));
}

void Gateway::connectToOPCUAServer()
{
    // const static QUrl opcuaServer(QLatin1String("opc.tcp://172.19.80.34:4840"));
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
    connect(reply, &QNetworkReply::finished, this, [this, reply]
    {
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject obj = doc.object();

        int authCode = obj.value(QLatin1String("result")).toInt();
        displayUserName = obj.value(QLatin1String("displayName")).toString();
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
            QString toSent = QString("{'OperatorLogin': 1, 'UserName': '%1'}").arg(displayUserName);
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
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
        emit readyToStartGateway();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Connected to RFID Scanner.");
        rfidThread = new QThread(this);
        rfidThread->start();

        rfidTimer = new QTimer();
        rfidTimer->setInterval(400);

        connect(rfidTimer, SIGNAL(timeout()), rfidTool, SLOT(icode2()), Qt::DirectConnection);
        connect(rfidThread, SIGNAL(finished()), rfidTimer, SLOT(stop()));

        rfidTimer->start();
        //run timer work(loop reading RFID tag) in thread
        rfidTimer->moveToThread(rfidThread);
    }
    else
    {
        isRFIDStart = false;
        emit readyToStartGateway();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Fail to connect RFID Scanner.");
    }
}

void Gateway::closeRFID()
{
    rfidThread->quit();
    rfidThread->wait();

    rfidTool->closeDevice();
    isRFIDStart = false;
    emit readyToStartGateway();

    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                            + "Disconnect to RFID Scanner.");
}

void Gateway::connectMqtt()
{
    mqttClient = new MqttClient(this);
    connect(mqttClient, &MqttClient::sendConState, this, [this](int state)
    {
        if (state == 1)
        {
            isMqttConnected = true;
            emit readyToStartGateway();
            // get job list information w.r.t rfid card
            mqttClient->subscribe("v1/devices/me/rpc/request/+", 0);
            connect(mqttClient, SIGNAL(sendSubMsg(QString, QString)), this, SLOT(receiveMqttSubMsg(QString, QString)));
            ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                      + "Connected to Mqtt broker.");
        }
        else
        {
            isMqttConnected = false;
            emit readyToStartGateway();
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

    jobIDNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_ID"); //string
    jobNameNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_ProcessName"); //string
    materialCodeNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_MaterialCode"); // string
    jobRecipeNameNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_RecipeName"); // string
    jobPlanQtyNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_PlanQty");  // int32
    jobPlanStartTimeNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_PlanStartTime"); //string
    jobPlanEndTimeNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_PlanEndTime");  // string
    conveyorSpeedNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.parameters.conveyor_Speed"); //int16
    jobApproveNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job_approve");  // int16

    materialReadyNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.materialReady"); // uint16

    jobModelNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_Model"); // string
    jobLengthNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_Length"); // int 16
    jobColorNodeW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job.job_Color"); // string

    objectPresentNodeRW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.objectPresent"); // uint 16
    objectPresentNodeRW->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(objectPresentNodeRW, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read objectPresent status node:" << value.toInt();
        ui->labelObjectPresent->setNum(value.toInt());

        if (isMqttConnected && value.toInt() == 1)
        {
            QString toSent = QString("{'JobID': %1, 'ObjectPresent': '1'}").arg(sJobID);
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
            objectPresentNodeRW->writeAttribute(QOpcUa::NodeAttribute::Value, 0, QOpcUa::UInt16);
        }
    });

    userLogoutNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.userLogout"); // unit16
    userLogoutNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(userLogoutNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read userLogout status node:" << value.toInt();

        if (isMqttConnected && value.toInt() == 1)
        {
            QString toSent = QString("{'OperatorLogout': 1, 'UserName': '%1'}").arg(displayUserName);
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
            ui->labelID->clear();
            ui->labelAccessLevel->clear();
            ui->labelApprove->clear();
            ui->labelPassword->clear();
            ui->labelName->clear();
            ui->labelTimeLogin->clear();        }
    });

    machineStepNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.machineStep"); // uint 16
    machineStepNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(machineStepNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read machineStep status node:" << value.toInt();
        ui->labelMachineStep->setNum(value.toInt());
        if (isMqttConnected)
        {
            QString toSent = QString("{'JobID': %1,'MachineStep': %2}").arg(sJobID, QString::number(value.toInt()));
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        }
    });

    jobCompletedNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job_completed"); // int16
    jobCompletedNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(jobCompletedNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read jobCompleted status node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Job Completed Status in OPCUA server updated: " + QString::number(value.toInt()));
        ui->labelJobCompletedStatus->setNum(value.toInt());

        if (value.toInt() == 1)
        {
            sJobID = "NA";
            ui->labelRFIDRead->clear();
            ui->labelJobID->clear();
            ui->labelJobProcess->clear();
            ui->labelJobMaterial->clear();
            ui->labelJobRecipe->clear();
            ui->labelJobQuantity->clear();
            ui->labelJobStartTime->clear();
            ui->labelJobEndTime->clear();
            ui->labelJobConSpeed->clear();
        }

    });

    jobBusyStatusNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.job_busy"); // int16
    jobBusyStatusNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(jobBusyStatusNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read jobBusy status node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Job Busy Status in OPCUA server updated: " + QString::number(value.toInt()));
        ui->labelJobBusyStatus->setNum(value.toInt());
        if (value.toInt() == 1)
        {
            isJobStart = true;
            isJobCompleted = false;
        }
        else if (value.toInt() == 0)
        {
            isJobStart = false;
            isJobCompleted = true;
        }
    });

    machineReadyNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.vision.MACHINE_READY"); // uint16
    machineReadyNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(machineReadyNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read machineReady node:" << value.toInt();
        ui->labelMachineReady->setNum(value.toInt());

        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Machine Ready(for vision result writing) in OPCUA server updated: " + QString::number(value.toInt()));
    });

    resultReadNodeRW = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.vision.RESULT_READ"); // uint16
    resultReadNodeRW->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(resultReadNodeRW, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read resultRead node:" << value.toInt();
        ui->labelResultRead->setNum(value.toInt());

        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Result Read Status in OPCUA server updated: " + QString::number(value.toInt()));
        if (value.toInt() == 1)
        {
            isResultRead = true;
            emit readyToResetVisionResult();
        }
        else
        {
            isResultRead = false;
        }

    });

    visionResultR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.vision.RESULT"); // uint16
    visionResultR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(visionResultR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read visionResult node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Vision Result in OPCUA server updated: " + QString::number(value.toInt()));
        if ( isJobStart && (value.toInt() != 0) )
        {
            ui->labelJobVisionResult->setNum(value.toInt());
            ui->labelJobTime->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
            QString toSent = QString("{'JobID': %1, 'VisionResult': %2}").arg(sJobID, QString::number(value.toInt()));
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
            isResultPublished = true;
            emit readyToResetVisionResult();
        }
    });

    goodPartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.good_parts"); // int32
    goodPartsCounterR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(goodPartsCounterR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read good parts Counter node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Good parts counter in OPCUA server updated: " + QString::number(value.toInt()));
        ui->labelJobGoodCounter->setNum(value.toInt());
        if (isJobStart)
        {
            QString toSent = QString("{'JobID': %1, 'GoodPartsCounter': %2}").arg(sJobID, QString::number(value.toInt()));
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        }
    });

    rejectSizePartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.rejectSize_parts"); //int32
    rejectSizePartsCounterR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(rejectSizePartsCounterR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read reject-size parts Counter node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Reject-size parts counter in OPCUA server updated: " + QString::number(value.toInt()));
        ui->labelJobSizeRejCounter->setNum(value.toInt());
        if (isJobStart)
        {
            QString toSent = QString("{'JobID': %1, 'RejectSizePartsCounter': %2}").arg(sJobID, QString::number(value.toInt()));
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        }
    });

    rejectColorPartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.rejectColor_parts"); // int32
    rejectColorPartsCounterR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(rejectColorPartsCounterR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read reject-color parts Counter node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Reject-color parts counter in OPCUA server updated: " + QString::number(value.toInt()));
        ui->labelJobColorRejCounter->setNum(value.toInt());
        if (isJobStart)
        {
            QString toSent = QString("{'JobID': %1, 'RejectColorPartsCounter': %2}").arg(sJobID, QString::number(value.toInt()));
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        }
    });

    totalPartsCounterR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.counter.total_parts"); //int32
    totalPartsCounterR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(totalPartsCounterR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read total parts Counter node:" << value.toInt();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Total parts counter in OPCUA server updated: " + QString::number(value.toInt()));
        ui->labelJobTotalCounter->setNum(value.toInt());
        if (isJobStart)
        {
            QString toSent = QString("{'JobID': %1, 'TotalPartsCounter': %2}").arg(sJobID, QString::number(value.toInt()));
            mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        }
    });

    ///////////////////////////////////////////////Job Request, Vision Status, Power Status////////////////////////////////////////////
    powerStatusNodeR = opcuaClient->node("ns=2;s=|var|CPS-PCS341MB-DS1.Application.GVL.OPC_Machine_A0001.power_status"); // uint16
    powerStatusNodeR->enableMonitoring(QOpcUa::NodeAttribute::Value, QOpcUaMonitoringParameters(100));
    connect(powerStatusNodeR, &QOpcUaNode::attributeUpdated, this, [this](QOpcUa::NodeAttribute attr, const QVariant &value)
    {
        Q_UNUSED(attr);
        qDebug() << "Read powerStatus node:" << value.toInt();
        ui->labelPowerStatus->setNum(value.toInt());
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Power Status in OPCUA server updated: " + QString::number(value.toInt()));
        if (value.toInt() == 1)
        {
            if (isMqttConnected)
            {
                QString toSent = QString("{'PowerOn': 1}");
                mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
            }
            isPowerReady = true;
            //emit readyToSendJobRequest();
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
                                          + "Vision Status in OPCUA server updated: " + QString::number(value.toInt()));
        if (value.toInt() == 1)
        {
            if (isMqttConnected)
            {
                QString toSent = QString("{'VisionReady': 1}");
                mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
            }
            isVisionReady = true;
            //emit readyToSendJobRequest();
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
                                          + "Job Request in OPCUA server updated: " + QString::number(value.toInt()));
        if (value.toInt() == 1)
        {
            if (isMqttConnected)
            {
                QString toSent = QString("{'JobRequest': 1}");
                mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
            }
            isJobRequest = true;
            //emit readyToSendJobRequest();
        }
        else
        {
            isJobRequest = false;
        }
    });
}

void Gateway::opcuaDisconnected()
{
    isOpcUaConnected = false;
    opcuaClient->deleteLater();

    ui->labelID->clear();
    ui->labelPassword->clear();
    ui->labelTimeLogin->clear();
    ui->labelApprove->clear();
    ui->labelName->clear();
    ui->labelAccessLevel->clear();
    ui->labelPowerStatus->clear();
    ui->labelVisionStatus->clear();
    ui->labelJobRequest->clear();
    ui->labelMachineStep->clear();
    ui->labelJobBusyStatus->clear();
    ui->labelJobCompletedStatus->clear();
    ui->labelJobVisionResult->clear();
    ui->labelJobGoodCounter->clear();
    ui->labelJobTotalCounter->clear();
    ui->labelJobColorRejCounter->clear();
    ui->labelJobSizeRejCounter->clear();
    ui->labelJobTime->clear();
    ui->labelMachineReady->clear();
    ui->labelResultRead->clear();
    ui->labelObjectPresent->clear();
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
        qDebug() << "Connected to OPCUA server.";
        isOpcUaConnected = true;
        emit readyToStartGateway();
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                                  + "Connected to OPCUA server.");
    }
    else if (state == QOpcUaClient::ClientState::Connecting)
    {
        qDebug() << "Trying to connect OPCUA server now.";
        isOpcUaConnected = false;
        emit readyToStartGateway();
    }
    else if (state == QOpcUaClient::ClientState::Disconnected)
    {
        qDebug() << "Disconnected to OPCUA server.";
        isOpcUaConnected = false;
        emit readyToStartGateway();
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
        isMaterialReady = true;
        QString toSent = QString("{'JobID': %1, 'MaterialReady': 1, 'TagID': %2}").arg(sJobID ,card);
        mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        materialReadyNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, 1, QOpcUa::UInt16);

        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + card + ": " +  data.split(":")[2]);
        ui->labelRFIDRead->setText(card);
        cardID = card;
        //emit readyToSendJobRequest();
    }
    else
    {
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + card);
    }
}

void Gateway::receiveMqttSubMsg(QString topic, QString msg)
{
    ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                      + "Msg from mqtt:" + msg.replace("[","").replace("]",""));

    if (topic.contains("v1/devices/me/rpc/request", Qt::CaseInsensitive))
    {
        const QJsonDocument doc = QJsonDocument::fromJson(msg.replace("[","").replace("]","").toUtf8());
        const QJsonObject obj = doc.object();

        QString jobID = obj.value(QLatin1String("params")).toObject().value(QLatin1String("job_id")).toString();
        sJobID = jobID;
        QString jobName = obj.value(QLatin1String("params")).toObject().value(QLatin1String("job")).toString();
        QString materialCode = obj.value(QLatin1String("params")).toObject().value(QLatin1String("material_code")).toString();
        QString jobRecipeName = obj.value(QLatin1String("params")).toObject().value(QLatin1String("recipe_name")).toString();
        QString jobPlanQty = obj.value(QLatin1String("params")).toObject().value(QLatin1String("quantity")).toString();
        QString jobPlanStartTime = obj.value(QLatin1String("params")).toObject().value(QLatin1String("planned_start_time")).toString();
        QString jobPlanEndTime = obj.value(QLatin1String("params")).toObject().value(QLatin1String("planned_end_time")).toString();
        QString conveyorSpeed = obj.value(QLatin1String("params")).toObject().value(QLatin1String("conveyer_speed")).toString();

        QString jobModel =obj.value(QLatin1String("params")).toObject().value(QLatin1String("model")).toString();
        QString jobLength =obj.value(QLatin1String("params")).toObject().value(QLatin1String("length")).toString();
        QString jobColor =obj.value(QLatin1String("params")).toObject().value(QLatin1String("color")).toString();

        ui->labelJobID->setText(jobID);
        ui->labelJobProcess->setText(jobName);
        ui->labelJobMaterial->setText(materialCode);
        ui->labelJobRecipe->setText(jobRecipeName);
        ui->labelJobQuantity->setText(jobPlanQty);
        ui->labelJobStartTime->setText(QDateTime::fromMSecsSinceEpoch(jobPlanStartTime.toLongLong()).toString("yyyy-MM-dd hh:mm:ss"));
        ui->labelJobEndTime->setText(QDateTime::fromMSecsSinceEpoch(jobPlanEndTime.toLongLong()).toString("yyyy-MM-dd hh:mm:ss"));
        ui->labelJobConSpeed->setText(conveyorSpeed);
        ui->labelJobModel->setText(jobModel);
        ui->labelJobSize->setText(jobLength);
        ui->labelJobColor->setText(jobColor);

        jobIDNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobID, QOpcUa::String);
        jobNameNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobName, QOpcUa::String);
        materialCodeNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, materialCode, QOpcUa::String);
        jobRecipeNameNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobRecipeName, QOpcUa::String);
        jobPlanQtyNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobPlanQty.toInt(), QOpcUa::Int32);
        jobPlanStartTimeNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, QDateTime::fromMSecsSinceEpoch(jobPlanStartTime.toLongLong()).toString("yyyy-MM-dd hh:mm:ss"), QOpcUa::String);
        jobPlanEndTimeNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, QDateTime::fromMSecsSinceEpoch(jobPlanEndTime.toLongLong()).toString("yyyy-MM-dd hh:mm:ss"), QOpcUa::String);
        conveyorSpeedNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, conveyorSpeed.toInt(), QOpcUa::Int16);

        jobApproveNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, 8, QOpcUa::Int16); // Approve job request (8),reject(7)
        jobModelNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobModel, QOpcUa::String);
        jobColorNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, jobColor, QOpcUa::String);
        jobLengthNodeW->writeAttribute(QOpcUa::NodeAttribute::Value, int(jobLength.toDouble()), QOpcUa::Int16);

        ui->labelRFIDRead->clear();

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
        QString toSent = QString("{'JobRequest': 1, 'TagID': %1}").arg(cardID);
        mqttClient->publish("v1/devices/me/telemetry", toSent, 0);
        isMaterialReady = false;
        ui->listWidget->addItem("[Info]    " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss    ")
                                          + "Check status ok, sent job request to Thingsboard.");
    }
}

void Gateway::prepareToStartGateway()
{
    if (isOpcUaConnected && isMqttConnected && isRFIDStart)
    {
        isGatewayReady = true;
        ui->pushButtonStart->setDisabled(true);
        ui->pushButtonStop->setEnabled(true);
        ui->pushButtonStart->setStyleSheet("background-color: rgb(100, 255, 100);"); // green
    }
    else
    {
        isGatewayReady = false;
        ui->pushButtonStop->setDisabled(true);
        ui->pushButtonStart->setEnabled(true);
        ui->pushButtonStart->setStyleSheet("background-color: rgb(225, 225, 225);"); // gray
    }
}

void Gateway::prepareToResetVisionResult()
{
    if (isResultRead && isResultPublished)
    {
        resultReadNodeRW->writeAttribute(QOpcUa::NodeAttribute::Value, 0, QOpcUa::UInt16);
        visionResultR->writeAttribute(QOpcUa::NodeAttribute::Value, 0, QOpcUa::UInt16);
        isResultPublished = false;
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
        ui->labelJobRequest->clear();
        ui->labelMachineStep->clear();
        ui->labelJobBusyStatus->clear();
        ui->labelJobCompletedStatus->clear();
        ui->labelJobVisionResult->clear();
        ui->labelJobGoodCounter->clear();
        ui->labelJobTotalCounter->clear();
        ui->labelJobColorRejCounter->clear();
        ui->labelJobSizeRejCounter->clear();
        ui->labelJobTime->clear();
        ui->labelMachineReady->clear();
        ui->labelResultRead->clear();
        ui->labelObjectPresent->clear();
        connectToOPCUAServer();
    }

    if (!isMqttConnected)
    {
        ui->labelJobID->clear();
        ui->labelJobProcess->clear();
        ui->labelJobMaterial->clear();
        ui->labelJobRecipe->clear();
        ui->labelJobQuantity->clear();
        ui->labelJobStartTime->clear();
        ui->labelJobEndTime->clear();
        ui->labelJobConSpeed->clear();
        ui->labelJobModel->clear();
        ui->labelJobSize->clear();
        ui->labelJobColor->clear();
        connectMqtt();
    }

    if (!isRFIDStart)
    {
        ui->labelRFIDRead->clear();
        startRFID();
    }
}

void Gateway::on_pushButtonStop_clicked()
{
    if (isOpcUaConnected)
        diconnectToOPCUAServer();
    if (isMqttConnected)
        disconnectMqtt();
    if (isRFIDStart)
        closeRFID();

    isRFIDStart = false;
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
    ui->labelJobRequest->clear();
    ui->labelMachineStep->clear();
    ui->labelJobID->clear();
    ui->labelJobProcess->clear();
    ui->labelJobMaterial->clear();
    ui->labelJobRecipe->clear();
    ui->labelJobQuantity->clear();
    ui->labelJobStartTime->clear();
    ui->labelJobEndTime->clear();
    ui->labelJobConSpeed->clear();
    ui->labelJobBusyStatus->clear();
    ui->labelJobCompletedStatus->clear();
    ui->labelJobVisionResult->clear();
    ui->labelJobGoodCounter->clear();
    ui->labelJobTotalCounter->clear();
    ui->labelJobColorRejCounter->clear();
    ui->labelJobSizeRejCounter->clear();
    ui->labelJobTime->clear();
    ui->labelJobModel->clear();
    ui->labelJobSize->clear();
    ui->labelJobColor->clear();
    ui->labelMachineReady->clear();
    ui->labelResultRead->clear();
    ui->labelObjectPresent->clear();
}
