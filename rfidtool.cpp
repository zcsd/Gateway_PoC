#include "rfidtool.h"

RFIDTool::RFIDTool(QObject *parent) : QObject(parent)
{

}

bool RFIDTool::initDevice()
{
    int st;
    unsigned char szVer[128];
    int iNode = 0;
    char szNode[128];

    //  serila port  /dev/ttyUSB%d
    //  usb port     /dev/usb/hiddev%d
    do
    {
        sprintf(szNode, "/dev/ttyUSB%d", iNode);
        iNode++;
        if((icdev = fw_init_ex(1, szNode, 115200))== -1)
        {
            qDebug() << "fw_init_ex ERR" << icdev;
            return false;
        }
        else
        {
            st = fw_getver(icdev, szVer);
            if (st == 0)
            {
                break;
            }
            else
            {
                qDebug() << szNode << "init error";
                return false;
            }
        }
    } while (icdev != -1);

    //qDebug() << szNode << "init ok";
    emit sendDeviceInfo(szNode);
    fw_beep(icdev, 10);

    return true;
}

void RFIDTool::closeDevice()
{
    fw_beep(icdev, 20);
    fw_exit(icdev);
}

bool RFIDTool::testDevice()
{
    int st; //the state of each operation
    unsigned char rdata[1024];
    unsigned char wdata[1024];
    int rwlen = 200;
    int i;

    for(i=0; i< rwlen; i++)
        wdata[i] = 1 + i;

    st = fw_swr_eeprom(icdev, 0, rwlen, wdata);

    if (st)
    {
        //printf("fw_swr_eeprom error:0x%x\n", st);
        qDebug() << "fw_swr_eeprom error:0x" << st;
        goto DO_EXIT;
    }

    st = fw_srd_eeprom(icdev, 0, rwlen, rdata);

    if (st)
    {
        qDebug() << "fw_srd_eep error:0x" << st;
        //printf("fw_srd_eep error:0x%x\n",st);
        goto DO_EXIT;
    }

    qDebug() << "fw_srd_eep ok";
    fw_beep(icdev, 10);

    return true;
/*
    for(i = 0; i < rwlen; i++)
        printf("%02X ", rdata[i]);
*/
DO_EXIT:
    return false;
}

void RFIDTool::icode2()
{
    int st;
    unsigned char rlen[17]={0};
    unsigned char rbuffer[256];
    unsigned char szCardSn[512] ={0};
    unsigned char UID[16];
    unsigned char m_StaAddr = 12;
    unsigned char m_Blockno = 1;
    unsigned char tmp[256];
    int i;

    fw_config_card(icdev, 0x31);

    st= fw_inventory(icdev, 0x36, 0, 0, rlen,rbuffer); //find single card

    if (st)
    {
        //qDebug() << "Find single card ERROR!";
        //emit sendReadInfo(false, "Find single card ERROR", "");
        return;
    }

    hex_a(szCardSn, &rbuffer[0], 2 * rlen[0]);

    //qDebug() << "Find card" << (char *)szCardSn;

    memcpy(UID, (char*)&rbuffer[0], 8);

    st = fw_select_uid(icdev, 0x22, &UID[0]);
    if (st)
    {
        //qDebug() << "fw_select_uid ERROR!";
        //emit sendReadInfo(false, "fw_select_uid ERROR", "");
        return;
    }

    st = fw_reset_to_ready(icdev, 0x22, &UID[0]);
    if (st)
    {
        //qDebug() << "fw_reset_to_ready ERROR!";
        //emit sendReadInfo(false, "fw_reset_to_ready ERROR", "");
        return;
    }

    st = fw_get_securityinfo(icdev, 0x22, 0x04, 0x02, &UID[0], rlen, rbuffer);
    if (st)
    {
        //qDebug() << "fw_get_securityinfo ERROR!";
        //emit sendReadInfo(false, "fw_get_securityinfo ERROR", "");
        return;
    }

    st = fw_readblock(icdev, 0x22, m_StaAddr, m_Blockno, &UID[0], rlen, rbuffer);  //read block data
    if (st)
    {
        //qDebug() << "Read data ERROR!";
        //emit sendReadInfo(false, "Read data ERROR", "");
        return;
    }

    for (i = 0; i < m_Blockno; i++)
    {
        sprintf((char *)tmp,"BlockAddr:[%2d] Data:[%02X %02X %02X %02X]",m_StaAddr+i,rbuffer[i*4],rbuffer[i*4+1],rbuffer[i*4+2],rbuffer[i*4+3]);
        //qDebug() << (const char *)tmp;
        emit sendReadInfo(true, (char *)szCardSn, (const char *)tmp);
        fw_beep(icdev, 10);
    }
}
