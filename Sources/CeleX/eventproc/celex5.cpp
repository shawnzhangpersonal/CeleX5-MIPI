/*
* Copyright (c) 2017-2018  CelePixel Technology Co. Ltd.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "../include/celex5/celex5.h"
#include "../frontpanel/frontpanel.h"
#include "../driver/CeleDriver.h"
#include "../configproc/hhsequencemgr.h"
#include "../configproc/hhwireincommand.h"
#include "../base/xbase.h"
#include "../eventproc/dataprocessthread.h"
#include "datarecorder.h"
#include <iostream>
#include <cstring>
#include <thread>

#ifdef _WIN32
static HANDLE s_hEventHandle = nullptr;
#endif

#define CSR_COL_GAIN          45

#define CSR_BIAS_ADVL_I_H     30
#define CSR_BIAS_ADVL_I_L     31
#define CSR_BIAS_ADVH_I_H     26
#define CSR_BIAS_ADVH_I_L     27

#define CSR_BIAS_ADVCL_I_H    38
#define CSR_BIAS_ADVCL_I_L    39
#define CSR_BIAS_ADVCH_I_H    34
#define CSR_BIAS_ADVCH_I_L    35

CeleX5::CeleX5()
	: m_bLoopModeEnabled(false)
	, m_uiClockRate(100)
	, m_uiLastClockRate(100)
	, m_iEventDataFormat(2)
	, m_pDataToRead(NULL)
	, m_uiPackageCount(0)
	, m_uiTotalPackageCount(0)
	, m_bFirstReadFinished(false)
	, m_pReadBuffer(NULL)
	, m_emDeviceType(CeleX5::Unknown_Devive)
	, m_pFrontPanel(NULL)
	, m_pCeleDriver(NULL)
	, m_uiPackageCounter(0)
	, m_uiPackageTDiff(0)
	, m_uiPackageBeginT(0)
	, m_bAutoISPEnabled(false)
	, m_uiBrightness(100)
	, m_uiThreshold(171)
	, m_arrayISPThreshold{ 60, 500, 2500 }
	, m_arrayBrightness{ 100, 110, 120, 130 }
	, m_uiAutoISPRefreshTime(80)
	, m_uiISOLevel(2)
	, m_uiOpticalFlowFrameTime(30)
	, m_bHasIMUData(true)
	, m_bClockAutoChanged(false)
	, m_uiISOLevelCount(4)
	, m_iRotateType(0)
{
	m_pSequenceMgr = new HHSequenceMgr;
	//create data process thread
	m_pDataProcessThread = new DataProcessThreadEx("CeleX5Thread");
	//m_pDataProcessThread->setDeviceType(CeleX5::CeleX5_MIPI);
	m_pDataProcessThread->setCeleX(this);
	//m_pDataProcessThread->start();

	m_pDataRecorder = new DataRecorder;
	//auto n = thread::hardware_concurrency();//cpu core count = 8
}

CeleX5::~CeleX5()
{
	if (m_pDataProcessThread)
	{
		if (m_pDataProcessThread->isRunning())
			m_pDataProcessThread->terminate();
		delete m_pDataProcessThread;
		m_pDataProcessThread = NULL;
	}
	if (m_pFrontPanel)
	{
		delete m_pFrontPanel;
		m_pFrontPanel = NULL;
	}
	if (m_pCeleDriver)
	{
		m_pCeleDriver->clearData();
		m_pCeleDriver->Close();
		delete m_pCeleDriver;
		m_pCeleDriver = NULL;
	}
	if (m_pSequenceMgr) delete m_pSequenceMgr;
	//
	if (m_pDataRecorder) delete m_pDataRecorder;


	if (m_pReadBuffer) {
		delete[] m_pReadBuffer;
		m_pReadBuffer = NULL;
	}
}

bool CeleX5::openSensor(DeviceType type)
{
	m_emDeviceType = type;
	m_pDataProcessThread->setDeviceType(type);
	if (CeleX5::CeleX5_OpalKelly == type)
	{
		m_pSequenceMgr->parseCeleX5Cfg(FILE_CELEX5_CFG_PA);
		m_mapCfgModified = m_mapCfgDefaults = getCeleX5Cfg();
		m_pFrontPanel = FrontPanel::getInstance();
		initializeFPGA();
		if (isSensorReady())
		{
			if (!configureSettings(type))
				return false;
			/*if (m_pFPGAReaderThread == NULL)
			{
				m_pFPGAReaderThread = new DataReaderThread(this);
				m_pFPGAReaderThread->startReadData(true);
				m_pFPGAReaderThread->start();
			}*/
			m_pDataProcessThread->start();
		}
	}
	else if (CeleX5::CeleX5_MIPI == type)
	{
		if (NULL == m_pCeleDriver)
		{
			m_pCeleDriver = new CeleDriver;
			//if (!m_pCeleDriver->Open())
			if (!m_pCeleDriver->openUSB())
			{
				m_pSequenceMgr->parseCeleX5Cfg(FILE_CELEX5_CFG_MIPI);
				m_mapCfgModified = m_mapCfgDefaults = getCeleX5Cfg();
				return false;
			}
			//m_pCeleDriver->writeSerialNumber("CX5-MP-0001-HXQ");
			//Sleep(1000);
		}
		std::string serialNumber = getSerialNumber();
		if (serialNumber.size() > 4 && serialNumber.at(4) == 'M') //no wire version
		{
			m_pSequenceMgr->parseCeleX5Cfg(FILE_CELEX5_CFG_MIPI);
			m_uiISOLevel = 2;
			m_uiISOLevelCount = 4;
			m_uiBrightness = 130;
		}
		else //wire version
		{
			m_pSequenceMgr->parseCeleX5Cfg(FILE_CELEX5_CFG_MIPI_WRIE);
			m_uiISOLevel = 3;
			m_uiISOLevelCount = 6;
			m_uiBrightness = 100;
		}
		m_pDataProcessThread->getDataProcessor5()->setISOLevel(m_uiISOLevel);
		m_mapCfgModified = m_mapCfgDefaults = getCeleX5Cfg();

		if (!configureSettings(type))
			return false;
		/*if (m_pMIPIReaderThread == NULL)
		{
			m_pMIPIReaderThread = new MIPIDataReaderThread(this);
			m_pMIPIReaderThread->setReadDataType(true);
			m_pMIPIReaderThread->start();
		}*/
		clearData();
		m_pDataProcessThread->start();
	}
	return true;
}

bool CeleX5::isSensorReady()
{
	return true;
}

void CeleX5::pipeOutFPGAData()
{
	if (CeleX5::CeleX5_OpalKelly != m_emDeviceType)
	{
		return;
	}
	if (!isSensorReady())
	{
		return;
	}
	uint32_t pageCount;
	m_pFrontPanel->wireOut(0x21, 0x1FFFFF, &pageCount);
	//cout << "-------------- pageCount = " << pageCount << endl;
	if (pageCount > 10)
	{
		if (pageCount > MAX_PAGE_COUNT)
			pageCount = MAX_PAGE_COUNT;
		int blockSize = 128;
		long length = (long)(pageCount * blockSize);
		if (NULL == m_pReadBuffer)
			m_pReadBuffer = new unsigned char[128 * MAX_PAGE_COUNT];
		//unsigned char* pData = new unsigned char[length];
		//Return the number of bytes read or ErrorCode (<0) if the read failed. 
		long dataLen = m_pFrontPanel->blockPipeOut(0xa0, blockSize, length, m_pReadBuffer);
		if (dataLen > 0)
		{
			//record sensor data
			if (m_pDataRecorder->isRecording())
			{
				m_pDataRecorder->writeData(m_pReadBuffer, dataLen);
			}
			m_pDataProcessThread->addData(m_pReadBuffer, dataLen);
		}
		else if (dataLen < 0) //read failed
		{
			switch (dataLen)
			{
			case okCFrontPanel::InvalidBlockSize:
				cout << "Block Size Not Supported" << endl;
				break;

			case okCFrontPanel::UnsupportedFeature:
				cout << "Unsupported Feature" << endl;
				break;

			default:
				cout << "Transfer Failed with error: " << dataLen << endl;
				break;
			}
			cout << "pageCount = " << pageCount << ", blockSize = " << blockSize << ", length = " << length << endl;
		}
	}
}

void CeleX5::getMIPIData(vector<uint8_t> &buffer)
{
	if (CeleX5::CeleX5_MIPI != m_emDeviceType)
	{
		return;
	}
	if (m_pCeleDriver->getimage(buffer))
	{
		//record sensor data
		if (m_pDataRecorder->isRecording())
		{
			m_pDataRecorder->writeData(buffer);
		}
		//cout << "image buffer size = " << buffer.size() << endl;
		//m_pDataProcessThread->addData(buffer);

		if (!m_bLoopModeEnabled && getSensorFixedMode() > 2)
		{
			m_uiPackageCounter++;
#ifdef _WIN32
			uint32_t t2 = GetTickCount();
#else
			uint32_t t2 = clock() / 1000;
#endif
			m_uiPackageTDiff += (t2 - m_uiPackageBeginT);
			m_uiPackageBeginT = t2;
			if (m_uiPackageTDiff > 1000)
			{
				//cout << "--- package count = " << counter << endl;
				m_pDataProcessThread->getDataProcessor5()->getProcessedData()->setFullFrameFPS(m_uiPackageCountPS);
				m_uiPackageTDiff = 0;
				m_uiPackageCountPS = m_uiPackageCounter;
				m_uiPackageCounter = 0;
			}
		}
	}
}

void CeleX5::getMIPIData(vector<uint8_t> &buffer, std::time_t& time_stamp_end, vector<IMURawData>& imu_data)
{
	if (CeleX5::CeleX5_MIPI != m_emDeviceType)
	{
		return;
	}
	vector<IMU_Raw_Data> imu_raw_data;
	if (m_pCeleDriver->getSensorData(buffer, time_stamp_end, imu_raw_data))
	{
		imu_data = vector<IMURawData>(imu_raw_data.size());
		for (int i = 0; i < imu_raw_data.size(); i++)
		{
			//cout << "--------------"<<imu_raw_data[i].time_stamp << endl;
			memcpy(imu_data[i].imu_data, imu_raw_data[i].imu_data, sizeof(imu_raw_data[i].imu_data));
			imu_data[i].time_stamp = imu_raw_data[i].time_stamp;
			//imu_data[i] = ((struct IMURawData*)&imu_raw_data[i]);
		}
		//parseIMUData(imu_data);
		//record sensor data
		if (m_pDataRecorder->isRecording())
		{
			if (m_strFirmwareVer == "2.0" & !m_bLoopModeEnabled)
				m_pDataRecorder->writeData(buffer, time_stamp_end, imu_data);
			else
				m_pDataRecorder->writeData(buffer);
		}
		//cout << "image buffer size = " << buffer.size() << ", time_stamp1 = " << time_stamp1 << ", time_stamp2 = " << time_stamp2 << endl;
		//m_pDataProcessThread->addData(buffer);
		if (!m_bLoopModeEnabled && getSensorFixedMode() > 2)
		{
			m_uiPackageCounter++;
#ifdef _WIN32
			uint32_t t2 = GetTickCount();
#else
			uint32_t t2 = clock() / 1000;
#endif
			m_uiPackageTDiff += (t2 - m_uiPackageBeginT);
			m_uiPackageBeginT = t2;
			if (m_uiPackageTDiff > 1000)
			{
				//cout << "--- package count = " << counter << endl;
				m_pDataProcessThread->getDataProcessor5()->getProcessedData()->setFullFrameFPS(m_uiPackageCountPS);
				m_uiPackageTDiff = 0;
				m_uiPackageCountPS = m_uiPackageCounter;
				m_uiPackageCounter = 0;
			}
		}
	}
}

CeleX5::DeviceType CeleX5::getDeviceType()
{
	return m_emDeviceType;
}

bool CeleX5::setFpnFile(const std::string& fpnFile)
{
	return m_pDataProcessThread->getDataProcessor5()->setFpnFile(fpnFile);
}

void CeleX5::generateFPN(std::string fpnFile)
{
	m_pDataProcessThread->getDataProcessor5()->generateFPN(fpnFile);
}

void CeleX5::reset()
{
	if (m_emDeviceType == CeleX5::CeleX5_OpalKelly)
	{
		resetPin(true);
		resetPin(false);
		resetConfigureSettings(m_emDeviceType);
	}
	else if (m_emDeviceType == CeleX5::CeleX5_MIPI)
	{
		enterCFGMode();
		clearData();
		enterStartMode();
	}
}

void CeleX5::pauseSensor()
{
	enterCFGMode();
}

void CeleX5::restartSensor()
{
	enterStartMode();
}

uint32_t CeleX5::getFullFrameFPS()
{
	return m_uiPackageCountPS;
}

// Set the Sensor operation mode in fixed mode
// address = 53, width = [2:0]
void CeleX5::setSensorFixedMode(CeleX5Mode mode)
{
	//-------------- for test --------------
	if (CeleX5::Event_Intensity_Mode == mode || CeleX5::Event_Optical_Flow_Mode == mode)
	{
		if (m_uiClockRate > 70)
		{
			m_bClockAutoChanged = true;
			m_uiLastClockRate = m_uiClockRate;
			setClockRate(70);
		}		
	}
	else
	{
		if (m_bClockAutoChanged)
		{
			setClockRate(m_uiLastClockRate);
			m_bClockAutoChanged = false;
		}
	}
	//-------------- for test --------------

	m_pDataProcessThread->clearData();

	if (CeleX5::CeleX5_MIPI == m_emDeviceType)
	{
		m_pCeleDriver->clearData();
	}

	//Disable ALS read and write, must be the first operation
	setALSEnabled(false);

	//Enter CFG Mode
	wireIn(93, 0, 0xFF);
	wireIn(90, 1, 0xFF);

	//Write Fixed Sensor Mode
	wireIn(53, static_cast<uint32_t>(mode), 0xFF);

	//Disable brightness adjustment (auto isp), always load sensor core parameters from profile0
	wireIn(221, 0, 0xFF); //AUTOISP_BRT_EN
	wireIn(223, 0, 0xFF); //AUTOISP_TRIGGER
	wireIn(220, 0, 0xFF); //AUTOISP_PROFILE_ADDR, Write core parameters to profile0
	writeRegister(233, -1, 232, 1500); //AUTOISP_BRT_VALUE, Set initial brightness value 1500
	writeRegister(22, -1, 23, m_uiBrightness); //BIAS_BRT_I, Override the brightness value in profile0, avoid conflict with AUTOISP profile0

	if (CeleX5::Event_Intensity_Mode == mode || CeleX5::Event_Optical_Flow_Mode == mode)
	{
		m_iEventDataFormat = 1;
		wireIn(73, m_iEventDataFormat, 0xFF); //EVENT_PACKET_SELECT
		m_pDataProcessThread->getDataProcessor5()->setMIPIDataFormat(m_iEventDataFormat);
		/*
		(1) CSR_114 / CSR_115 = 96
		(2) CSR_74 = 254
		(3) CSR_76 / CSR_77 = 1280

		(4) CSR_79 / CSR_80 = 400
		(5) CSR_82 / CSR_83 = 800
		(6) CSR_84 / CSR_85 = 462
		(7) CSR_86 / CSR_87 = 1200
		*/
		writeRegister(79, -1, 80, 200); //has bug
		writeRegister(82, -1, 83, 800);
		writeRegister(84, -1, 85, 462);
		writeRegister(86, -1, 87, 1200);
	}
	else
	{
		m_iEventDataFormat = 2;
		wireIn(73, m_iEventDataFormat, 0xFF); //EVENT_PACKET_SELECT
		m_pDataProcessThread->getDataProcessor5()->setMIPIDataFormat(m_iEventDataFormat);
		/*
		(1) CSR_114 / CSR_115 = 96
		(2) CSR_74 = 254
		(3) CSR_76 / CSR_77 = 1280

		(4) CSR_79 / CSR_80 = 200
		(5) CSR_82 / CSR_83 = 720
		(6) CSR_84 / CSR_85 = 680
		(7) CSR_86 / CSR_87 = 1300
		*/
		writeRegister(79, -1, 80, 200);
		writeRegister(82, -1, 83, 720);
		writeRegister(84, -1, 85, 680);
		writeRegister(86, -1, 87, 1300);
	}
	//Enter Start Mode
	wireIn(90, 0, 0xFF);
	wireIn(93, 1, 0xFF);

	m_pDataProcessThread->getDataProcessor5()->setSensorFixedMode(mode);
}

// Set the Sensor operation mode in loop mode
// loop = 1: the first operation mode in loop mode, address = 53, width = [2:0]
// loop = 2: the second operation mode in loop mode, address = 54, width = [2:0]
// loop = 3: the third operation mode in loop mode, address = 55, width = [2:0]
void CeleX5::setSensorLoopMode(CeleX5Mode mode, int loopNum)
{
	if (loopNum < 1 || loopNum > 3)
	{
		cout << "CeleX5::setSensorMode: wrong loop number!";
		return;
	}
	clearData();

	enterCFGMode();
	wireIn(52 + loopNum, static_cast<uint32_t>(mode), 0xFF);
	enterStartMode();
	m_pDataProcessThread->getDataProcessor5()->setSensorLoopMode(mode, loopNum);
}

CeleX5::CeleX5Mode CeleX5::getSensorFixedMode()
{
	return m_pDataProcessThread->getDataProcessor5()->getSensorFixedMode();
}

CeleX5::CeleX5Mode CeleX5::getSensorLoopMode(int loopNum)
{
	return m_pDataProcessThread->getDataProcessor5()->getSensorLoopMode(loopNum);
}

// enable or disable loop mode
// address = 64, width = [0], 0: fixed mode / 1: loop mode
void CeleX5::setLoopModeEnabled(bool enable)
{
	m_bLoopModeEnabled = enable;

	if (CeleX5::CeleX5_MIPI == m_emDeviceType)
	{
		if (m_bAutoISPEnabled)
			setALSEnabled(false);
	}
	bool bChangeParameters = false;
	if (enable)
	{
		if (m_bClockAutoChanged)
		{
			setClockRate(m_uiLastClockRate);
			m_bClockAutoChanged = false;
			bChangeParameters = true;
		}
	}
	enterCFGMode();
	if (enable)
	{
		wireIn(64, 1, 0xFF);

		if (CeleX5::CeleX5_MIPI == m_emDeviceType)
		{
			//Disable brightness adjustment (auto isp), always load sensor core parameters from profile0
			wireIn(221, 0, 0xFF); //AUTOISP_BRT_EN, disable auto isp
			wireIn(223, 1, 0xFF); //AUTOISP_TRIGGER
			wireIn(220, 0, 0xFF); //AUTOISP_PROFILE_ADDR, Write core parameters to profile0
			writeRegister(233, -1, 232, 1500); //AUTOISP_BRT_VALUE, Set initial brightness value 1500
			writeRegister(22, -1, 23, m_uiBrightness); //BIAS_BRT_I, Override the brightness value in profile0, avoid conflict with AUTOISP profile0
		}

		if (bChangeParameters)
		{
			m_iEventDataFormat = 2;
			wireIn(73, m_iEventDataFormat, 0xFF); //EVENT_PACKET_SELECT
			m_pDataProcessThread->getDataProcessor5()->setMIPIDataFormat(m_iEventDataFormat);
			/*
			(1) CSR_114 / CSR_115 = 96
			(2) CSR_74 = 254
			(3) CSR_76 / CSR_77 = 1280

			(4) CSR_79 / CSR_80 = 200
			(5) CSR_82 / CSR_83 = 720
			(6) CSR_84 / CSR_85 = 680
			(7) CSR_86 / CSR_87 = 1300
			*/
			writeRegister(79, -1, 80, 200);
			writeRegister(82, -1, 83, 720);
			writeRegister(84, -1, 85, 680);
			writeRegister(86, -1, 87, 1300);
		}
	}
	else
	{
		wireIn(64, 0, 0xFF);
	}
	enterStartMode();
	m_pDataProcessThread->getDataProcessor5()->setLoopModeEnabled(enable);
}

bool CeleX5::isLoopModeEnabled()
{
	return m_bLoopModeEnabled;
}

// Set the duration of event mode (Mode_A/B/C) when sensor operates in loop mode
// low byte address = 57, width = [7:0]
// high byte address = 58, width = [1:0]
void CeleX5::setEventDuration(uint32_t value)
{
	enterCFGMode();

	value = value * 1000 / 655;

	uint32_t valueH = value >> 8;
	uint32_t valueL = 0xFF & value;

	wireIn(58, valueH, 0xFF);
	wireIn(57, valueL, 0xFF);

	enterStartMode();
}

// Set the mumber of pictures to acquire in Mode_D/E/F/G/H
// Mode_D: address = 59, width = [7:0]
// Mode_E: address = 60, width = [7:0]
// Mode_F: address = 61, width = [7:0]
// Mode_G: address = 62, width = [7:0]
// Mode_H: address = 63, width = [7:0]
void CeleX5::setPictureNumber(uint32_t num, CeleX5Mode mode)
{
	enterCFGMode();

	if (Full_Picture_Mode == mode)
		wireIn(59, num, 0xFF);
	else if (Full_Optical_Flow_S_Mode == mode)
		wireIn(60, num, 0xFF);
	else if (Full_Optical_Flow_M_Mode == mode)
		wireIn(62, num, 0xFF);

	enterStartMode();
}

// related to fps (main clock frequency), hardware parameter
uint32_t CeleX5::getFullPicFrameTime()
{
	return 1000 / m_uiClockRate;
}

//software parameter
void CeleX5::setEventFrameTime(uint32_t value)
{
	m_pDataProcessThread->getDataProcessor5()->setEventFrameTime(value, m_uiClockRate);
}
uint32_t CeleX5::getEventFrameTime()
{
	return m_pDataProcessThread->getDataProcessor5()->getEventFrameTime();
}

//hardware parameter
void CeleX5::setOpticalFlowFrameTime(uint32_t value)
{
	if (value <= 10 || value >= 180)
	{
		cout << __FUNCTION__ << ": value is out of range!" << endl;
		return;
	}	
	m_uiOpticalFlowFrameTime = value;
	//
	enterCFGMode();
	wireIn(169, (value - 10) * 3 / 2, 0xFF);
	enterStartMode();
}
uint32_t CeleX5::getOpticalFlowFrameTime()
{
	return m_uiOpticalFlowFrameTime;
}

void CeleX5::setEventShowMethod(EventShowType type, int value)
{
	m_pDataProcessThread->getDataProcessor5()->setEventShowMethod(type, value);
	if (type == EventShowByTime)
		setEventFrameTime(value);
}

// BIAS_EVT_VL : 341 address(2/3)
// BIAS_EVT_DC : 512 address(4/5)
// BIAS_EVT_VH : 683 address(6/7)
void CeleX5::setThreshold(uint32_t value)
{
	m_uiThreshold = value;

	enterCFGMode();

	int EVT_VL = 512 - value;
	if (EVT_VL < 0)
		EVT_VL = 0;
	writeRegister(2, -1, 3, EVT_VL);

	int EVT_VH = 512 + value;
	if (EVT_VH > 1023)
		EVT_VH = 1023;
	writeRegister(6, -1, 7, EVT_VH);

	enterStartMode();
}

uint32_t CeleX5::getThreshold()
{
	return m_uiThreshold;
}

// Set brightness
// <BIAS_BRT_I>
// high byte address = 22, width = [1:0]
// low byte address = 23, width = [7:0]
void CeleX5::setBrightness(uint32_t value)
{
	m_uiBrightness = value;

	enterCFGMode();
	writeRegister(22, -1, 23, value);
	enterStartMode();
}

uint32_t CeleX5::getBrightness()
{
	return m_uiBrightness;
}

uint32_t CeleX5::getClockRate()
{
	return m_uiClockRate;
}

void CeleX5::setClockRate(uint32_t value)
{
	if (value > 100 || value < 20)
		return;
	m_uiClockRate = value;
	if (CeleX5::CeleX5_OpalKelly == m_emDeviceType)
	{
		enterCFGMode();

		// Disable PLL
		wireIn(150, 0, 0xFF);
		// Write PLL Clock Parameter
		wireIn(159, value * 3 / 5, 0xFF);
		// Enable PLL
		wireIn(150, 1, 0xFF);

		enterStartMode();
	}
	else if (CeleX5::CeleX5_MIPI == m_emDeviceType)
	{
		if (value < 70)
		{
			m_bClockAutoChanged = false;
		}
		enterCFGMode();

		// Disable PLL
		wireIn(150, 0, 0xFF);
		int clock[15] = { 20,  30,  40,  50,  60,  70,  80,  90, 100, 110, 120, 130, 140, 150, 160 };

		int PLL_DIV_N[15] = { 12,  18,  12,  15,  18,  21,  12,  18,  15,  22,  18,  26,  21,  30, 24 };
		int PLL_DIV_L[15] = { 2,   3,   2,   2,   2,   2,   2,   3,   2,   3,   2,   3,   2,   3,  2 };
		int PLL_FOUT_DIV1[15] = { 3,   2,   1,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,  0 };
		int PLL_FOUT_DIV2[15] = { 3,   2,   3,   3,   3,   3,   3,   2,   3,   3,   3,   3,   3,   3,  3 };

		int MIPI_PLL_DIV_I[15] = { 3,   2,   3,   3,   2,   2,   3,   2,   3,   2,   2,   2,   2,   2,  1 };
		int MIPI_PLL_DIV_N[15] = { 120, 120, 120, 96,  120, 102, 120, 120, 96,  130, 120, 110, 102, 96, 120 };

		int index = value / 10 - 2;

		cout << "CeleX5::setClockRate: " << clock[index] << " MHz" << endl;

		// Write PLL Clock Parameter
		writeRegister(159, -1, -1, PLL_DIV_N[index]);
		writeRegister(160, -1, -1, PLL_DIV_L[index]);
		writeRegister(151, -1, -1, PLL_FOUT_DIV1[index]);
		writeRegister(152, -1, -1, PLL_FOUT_DIV2[index]);

		// Enable PLL
		wireIn(150, 1, 0xFF);

		disableMIPI();
		writeRegister(113, -1, -1, MIPI_PLL_DIV_I[index]);
		writeRegister(115, -1, 114, MIPI_PLL_DIV_N[index]);
		enableMIPI();

		enterStartMode();

		uint32_t frame_time = m_pDataProcessThread->getDataProcessor5()->getEventFrameTime();
		m_pDataProcessThread->getDataProcessor5()->setEventFrameTime(frame_time, m_uiClockRate);
	}
}

void CeleX5::setISOLevel(uint32_t value)
{
	if (6 == m_uiISOLevelCount)
	{
		int index = value - 1;
		if (index < 0)
			index = 0;
		if (index > 5)
			index = 5;

		m_uiISOLevel = index + 1;
		m_pDataProcessThread->getDataProcessor5()->setISOLevel(m_uiISOLevel);

		int col_gain[6] = { 2, 2, 2, 2, 1, 1 };
		int bias_advl_i[6] = { 470, 410, 350, 290, 410, 380 };
		int bias_advh_i[6] = { 710, 770, 830, 890, 770, 800 };
		int bias_advcl_i[6] = { 560, 545, 530, 515, 545, 540 };
		int bias_advch_i[6] = { 620, 635, 650, 665, 635, 640 };
		int bias_vcm_i[6] = { 590, 590, 590, 590, 590, 590 };

		enterCFGMode();

		writeRegister(CSR_COL_GAIN, -1, -1, col_gain[index]);
		writeRegister(CSR_BIAS_ADVL_I_H, -1, CSR_BIAS_ADVL_I_L, bias_advl_i[index]);
		writeRegister(CSR_BIAS_ADVH_I_H, -1, CSR_BIAS_ADVH_I_L, bias_advh_i[index]);
		writeRegister(CSR_BIAS_ADVCL_I_H, -1, CSR_BIAS_ADVCL_I_L, bias_advcl_i[index]);
		writeRegister(CSR_BIAS_ADVCH_I_H, -1, CSR_BIAS_ADVCH_I_L, bias_advch_i[index]);

		writeRegister(42, -1, 43, bias_vcm_i[index]);

		enterStartMode();
	}
	else
	{
		int index = value - 1;
		if (index < 0)
			index = 0;
		if (index > 3)
			index = 3;

		m_uiISOLevel = index + 1;
		m_pDataProcessThread->getDataProcessor5()->setISOLevel(m_uiISOLevel);

		int col_gain[4] = { 1, 1, 1, 1 };
		int bias_advl_i[4] = { 428, 384, 336, 284 };
		int bias_advh_i[4] = { 636, 680, 728, 780 };
		int bias_advcl_i[4] = { 506, 495, 483, 470 };
		int bias_advch_i[4] = { 558, 569, 581, 594 };
		int bias_vcm_i[4] = { 532, 532, 532, 532 };

		enterCFGMode();

		writeRegister(CSR_COL_GAIN, -1, -1, col_gain[index]);
		writeRegister(CSR_BIAS_ADVL_I_H, -1, CSR_BIAS_ADVL_I_L, bias_advl_i[index]);
		writeRegister(CSR_BIAS_ADVH_I_H, -1, CSR_BIAS_ADVH_I_L, bias_advh_i[index]);
		writeRegister(CSR_BIAS_ADVCL_I_H, -1, CSR_BIAS_ADVCL_I_L, bias_advcl_i[index]);
		writeRegister(CSR_BIAS_ADVCH_I_H, -1, CSR_BIAS_ADVCH_I_L, bias_advch_i[index]);

		writeRegister(42, -1, 43, bias_vcm_i[index]);

		enterStartMode();
	}
}

uint32_t CeleX5::getISOLevel()
{
	return m_uiISOLevel;
}

//0: format 0; 1: format 1; 2: format 2
void CeleX5::setEventDataFormat(int format)
{
	m_iEventDataFormat = format;
	wireIn(73, m_iEventDataFormat, 0xFF); //EVENT_PACKET_SELECT
	m_pDataProcessThread->getDataProcessor5()->setMIPIDataFormat(m_iEventDataFormat);
}

int CeleX5::getEventDataFormat()
{
	return m_iEventDataFormat;
}

void CeleX5::setRotateType(int type)
{
	m_iRotateType += type;
	m_pDataProcessThread->getDataProcessor5()->setRotateType(m_iRotateType);
}

int CeleX5::getRotateType()
{
	return m_iRotateType;
}

void CeleX5::setEventCountStepSize(uint32_t size)
{
	m_pDataProcessThread->getDataProcessor5()->setEventCountStep(size);
}

void CeleX5::getFullPicBuffer(unsigned char* buffer)
{
	m_pDataProcessThread->getDataProcessor5()->getFullPicBuffer(buffer);
}

cv::Mat CeleX5::getFullPicMat()
{
	CeleX5ProcessedData* pSensorData = m_pDataProcessThread->getDataProcessor5()->getProcessedData();
	if (pSensorData)
	{
		return cv::Mat(cv::Size(CELEX5_COL, CELEX5_ROW), CV_8UC1, pSensorData->getFullPicBuffer());
	}
	return cv::Mat();
}

void CeleX5::getEventPicBuffer(unsigned char* buffer, emEventPicType type)
{
	m_pDataProcessThread->getDataProcessor5()->getEventPicBuffer(buffer, type);
}

cv::Mat CeleX5::getEventPicMat(emEventPicType type)
{
	CeleX5ProcessedData* pSensorData = m_pDataProcessThread->getDataProcessor5()->getProcessedData();
	if (pSensorData)
	{
		return cv::Mat(cv::Size(CELEX5_COL, CELEX5_ROW), CV_8UC1, pSensorData->getEventPicBuffer(type));
	}
	return cv::Mat();
}

//void CeleX5::getOpticalFlowPicBuffer(unsigned char* buffer)
//{
//	m_pDataProcessThread->getDataProcessor5()->getOpticalFlowPicBuffer(buffer);
//}
//
//cv::Mat CeleX5::getOpticalFlowPicMat()
//{
//	CeleX5ProcessedData* pSensorData = m_pDataProcessThread->getDataProcessor5()->getProcessedData();
//	if (pSensorData)
//	{
//		return cv::Mat(cv::Size(CELEX5_COL, CELEX5_ROW), CV_8UC1, pSensorData->getOpticalFlowPicBuffer());
//	}
//	return cv::Mat();
//}

void CeleX5::getOpticalFlowPicBuffer(unsigned char* buffer, emFullPicType type)
{
	m_pDataProcessThread->getDataProcessor5()->getOpticalFlowPicBuffer(buffer, type);
}

cv::Mat CeleX5::getOpticalFlowPicMat(emFullPicType type)
{
	CeleX5ProcessedData* pSensorData = m_pDataProcessThread->getDataProcessor5()->getProcessedData();
	if (pSensorData)
	{
		return cv::Mat(cv::Size(CELEX5_COL, CELEX5_ROW), CV_8UC1, pSensorData->getOpticalFlowPicBuffer(type));
	}
	return cv::Mat();
}

bool CeleX5::getEventDataVector(std::vector<EventData> &vector)
{
	return m_pDataProcessThread->getDataProcessor5()->getEventDataVector(vector);
}

bool CeleX5::getEventDataVector(std::vector<EventData> &vector, uint64_t& frameNo)
{
	return m_pDataProcessThread->getDataProcessor5()->getEventDataVector(vector, frameNo);
}

bool CeleX5::getEventDataVectorEx(std::vector<EventData> &vector, std::time_t& time_stamp)
{
	return m_pDataProcessThread->getDataProcessor5()->getEventDataVectorEx(vector, time_stamp);
}

void CeleX5::startRecording(std::string filePath)
{
	m_strFirmwareVer = getFirmwareVersion();
	m_pDataProcessThread->setRecordState(true);
	m_pDataRecorder->startRecording(filePath);
}

void CeleX5::stopRecording()
{
	if (CeleX5::CeleX5_OpalKelly == m_emDeviceType)
	{
		m_pDataRecorder->stopRecording(25, 0);
	}
	else if (CeleX5::CeleX5_MIPI == m_emDeviceType)
	{
		BinFileAttributes header;
		if (isLoopModeEnabled())
		{
			header.data_type = 1;
			header.loopA_mode = m_pDataProcessThread->getDataProcessor5()->getSensorLoopMode(1);
			header.loopB_mode = m_pDataProcessThread->getDataProcessor5()->getSensorLoopMode(2);
			header.loopC_mode = m_pDataProcessThread->getDataProcessor5()->getSensorLoopMode(3);
		}
		else
		{
			if (m_strFirmwareVer == "2.0")
				header.data_type = 2;
			else
				header.data_type = 0;
			header.loopA_mode = m_pDataProcessThread->getDataProcessor5()->getSensorFixedMode();
			header.loopB_mode = 255;
			header.loopC_mode = 255;
		}
		header.event_data_format = m_iEventDataFormat;
		m_pDataRecorder->stopRecording(&header);
		m_pDataProcessThread->setRecordState(false);
	}
}

CX5SensorDataServer* CeleX5::getSensorDataServer()
{
	return m_pDataProcessThread->getDataProcessor5()->getSensorDataServer();
}

map<string, vector<CeleX5::CfgInfo>> CeleX5::getCeleX5Cfg()
{
	map<string, vector<HHCommandBase*>> mapCfg = m_pSequenceMgr->getCeleX5Cfg();
	//
	map<string, vector<CeleX5::CfgInfo>> mapCfg1;
	for (auto itr = mapCfg.begin(); itr != mapCfg.end(); itr++)
	{
		//cout << "CelexSensorDLL::getCeleX5Cfg: " << itr->first << endl;
		vector<HHCommandBase*> pCmdList = itr->second;
		vector<CeleX5::CfgInfo> vecCfg;
		for (auto itr1 = pCmdList.begin(); itr1 != pCmdList.end(); itr1++)
		{
			WireinCommandEx* pCmd = (WireinCommandEx*)(*itr1);
			//cout << "----- Register Name: " << pCmd->name() << endl;
			CeleX5::CfgInfo cfgInfo;
			cfgInfo.name = pCmd->name();
			cfgInfo.min = pCmd->minValue();
			cfgInfo.max = pCmd->maxValue();
			cfgInfo.value = pCmd->value();
			cfgInfo.high_addr = pCmd->highAddr();
			cfgInfo.middle_addr = pCmd->middleAddr();
			cfgInfo.low_addr = pCmd->lowAddr();
			vecCfg.push_back(cfgInfo);
		}
		mapCfg1[itr->first] = vecCfg;
	}
	return mapCfg1;
}

std::string CeleX5::getSerialNumber()
{
	//cout << "------------- Serial Number: " << m_pCeleDriver->getSerialNumber() << endl;
	return m_pCeleDriver->getSerialNumber();

}

std::string CeleX5::getFirmwareVersion()
{
	//cout << "------------- Firmware Version: " << m_pCeleDriver->getFirmwareVersion() << endl;
	return m_pCeleDriver->getFirmwareVersion();
}

std::string CeleX5::getFirmwareDate()
{
	//cout << "------------- Firmware Date: " << m_pCeleDriver->getFirmwareDate() << endl;
	return m_pCeleDriver->getFirmwareDate();
}

map<string, vector<CeleX5::CfgInfo>> CeleX5::getCeleX5CfgModified()
{
	return m_mapCfgModified;
}

void CeleX5::writeRegister(int16_t addressH, int16_t addressM, int16_t addressL, uint32_t value)
{
	if (addressL == -1)
	{
		wireIn(addressH, value, 0xFF);
	}
	else
	{
		if (addressM == -1)
		{
			uint32_t valueH = value >> 8;
			uint32_t valueL = 0xFF & value;
			wireIn(addressH, valueH, 0xFF);
			wireIn(addressL, valueL, 0xFF);
		}
	}
}

CeleX5::CfgInfo CeleX5::getCfgInfoByName(string csrType, string name, bool bDefault)
{
	map<string, vector<CfgInfo>> mapCfg;
	if (bDefault)
		mapCfg = m_mapCfgDefaults;
	else
		mapCfg = m_mapCfgModified;
	CeleX5::CfgInfo cfgInfo;
	for (auto itr = mapCfg.begin(); itr != mapCfg.end(); itr++)
	{
		string tapName = itr->first;
		if (csrType == tapName)
		{
			vector<CfgInfo> vecCfg = itr->second;
			int index = 0;
			for (auto itr1 = vecCfg.begin(); itr1 != vecCfg.end(); itr1++)
			{
				if ((*itr1).name == name)
				{
					cfgInfo = (*itr1);
					return cfgInfo;
				}
				index++;
			}
			break;
		}
	}
	return cfgInfo;
}

void CeleX5::writeCSRDefaults(string csrType)
{
	cout << "CeleX5::writeCSRDefaults: " << csrType << endl;
	m_mapCfgModified[csrType] = m_mapCfgDefaults[csrType];
	for (auto itr = m_mapCfgDefaults.begin(); itr != m_mapCfgDefaults.end(); itr++)
	{
		//cout << "group name: " << itr->first << endl;
		string tapName = itr->first;
		if (csrType == tapName)
		{
			vector<CeleX5::CfgInfo> vecCfg = itr->second;
			for (auto itr1 = vecCfg.begin(); itr1 != vecCfg.end(); itr1++)
			{
				CeleX5::CfgInfo cfgInfo = (*itr1);
				writeRegister(cfgInfo);
			}
			break;
		}
	}
}

void CeleX5::modifyCSRParameter(string csrType, string cmdName, uint32_t value)
{
	CeleX5::CfgInfo cfgInfo;
	for (auto itr = m_mapCfgModified.begin(); itr != m_mapCfgModified.end(); itr++)
	{
		string tapName = itr->first;
		if (csrType.empty())
		{
			vector<CfgInfo> vecCfg = itr->second;
			int index = 0;
			for (auto itr1 = vecCfg.begin(); itr1 != vecCfg.end(); itr1++)
			{
				if ((*itr1).name == cmdName)
				{
					cfgInfo = (*itr1);
					cout << "CeleX5::modifyCSRParameter: Old value = " << cfgInfo.value << endl;
					//modify the value in m_pMapCfgModified
					cfgInfo.value = value;
					vecCfg[index] = cfgInfo;
					m_mapCfgModified[tapName] = vecCfg;
					cout << "CeleX5::modifyCSRParameter: New value = " << cfgInfo.value << endl;
					break;
				}
				index++;
			}
		}
		else
		{
			if (csrType == tapName)
			{
				vector<CfgInfo> vecCfg = itr->second;
				int index = 0;
				for (auto itr1 = vecCfg.begin(); itr1 != vecCfg.end(); itr1++)
				{
					if ((*itr1).name == cmdName)
					{
						cfgInfo = (*itr1);
						cout << "CeleX5::modifyCSRParameter: Old value = " << cfgInfo.value << endl;
						//modify the value in m_pMapCfgModified
						cfgInfo.value = value;
						vecCfg[index] = cfgInfo;
						m_mapCfgModified[tapName] = vecCfg;
						cout << "CeleX5::modifyCSRParameter: New value = " << cfgInfo.value << endl;
						break;
					}
					index++;
				}
				break;
			}
		}
	}
}

bool CeleX5::initializeFPGA()
{
	return m_pFrontPanel->initializeFPGA("celex5_top.bit");
}

bool CeleX5::configureSettings(CeleX5::DeviceType type)
{
	if (CeleX5::CeleX5_OpalKelly == type)
	{
		//--------------- Step1 ---------------
		wireIn(94, 1, 0xFF);

		//--------------- Step2 ---------------
		//Disable PLL
		wireIn(150, 0, 0xFF);
		//Load PLL Parameters
		writeCSRDefaults("PLL_Parameters");
		//Enable PLL
		wireIn(150, 1, 0xFF);

		//--------------- Step3 ---------------
		enterCFGMode();

		//Load Other Parameters
		wireIn(92, 12, 0xFF); //

		//Load Sensor Core Parameters
		wireIn(220, 0, 0xFF); //AUTOISP_PROFILE_ADDR
		writeCSRDefaults("Sensor_Core_Parameters");

		//wireIn(91, 11, 0xFF);
		//wireIn(18, 151, 0xFF); //23

		//Load Sensor_Operation_Mode_Control_Parameters
		wireIn(53, 0, 0xFF); //SENSOR_MODE_1
		wireIn(54, 3, 0xFF); //SENSOR_MODE_2
		wireIn(55, 4, 0xFF); //SENSOR_MODE_3

		wireIn(57, 60, 0xFF); //EVENT_DURATION, low byte
		wireIn(58, 0, 0xFF); //EVENT_DURATION, high byte

		wireIn(64, 0, 0xFF); //SENSOR_MODE_SELECT, fixed mode

		enterStartMode();
	}
	else if (CeleX5::CeleX5_MIPI == type)
	{
		setALSEnabled(false);
		if (m_pCeleDriver)
			m_pCeleDriver->openStream();

		//--------------- Step1 ---------------
		wireIn(94, 0, 0xFF); //PADDR_EN

		//--------------- Step2: Load PLL Parameters ---------------
		//Disable PLL
		cout << "--- Disable PLL ---" << endl;
		wireIn(150, 0, 0xFF); //PLL_PD_B
		//Load PLL Parameters
		cout << endl << "--- Load PLL Parameters ---" << endl;
		writeCSRDefaults("PLL_Parameters");
		//Enable PLL
		cout << "--- Enable PLL ---" << endl;
		wireIn(150, 1, 0xFF); //PLL_PD_B

		//--------------- Step3: Load MIPI Parameters ---------------
		cout << endl << "--- Disable MIPI ---" << endl;
		disableMIPI();

		cout << endl << "--- Load MIPI Parameters ---" << endl;
		writeCSRDefaults("MIPI_Parameters");
		//writeRegister(115, -1, 114, 120); //MIPI_PLL_DIV_N

		//Enable MIPI
		cout << endl << "--- Enable MIPI ---" << endl;
		enableMIPI();

		//--------------- Step4: ---------------
		cout << endl << "--- Enter CFG Mode ---" << endl;
		enterCFGMode();

		//----- Load Sensor Core Parameters -----
		//if (m_bAutoISPEnabled)
		{
			//--------------- for auto isp --------------- 
			wireIn(220, 3, 0xFF); //AUTOISP_PROFILE_ADDR
			//Load Sensor Core Parameters
			writeCSRDefaults("Sensor_Core_Parameters");
			writeRegister(22, -1, 23, m_arrayBrightness[3]);

			wireIn(220, 2, 0xFF); //AUTOISP_PROFILE_ADDR
			 //Load Sensor Core Parameters
			writeCSRDefaults("Sensor_Core_Parameters");
			writeRegister(22, -1, 23, m_arrayBrightness[2]);

			wireIn(220, 1, 0xFF); //AUTOISP_PROFILE_ADDR
			//Load Sensor Core Parameters
			writeCSRDefaults("Sensor_Core_Parameters");
			writeRegister(22, -1, 23, m_arrayBrightness[1]);

			wireIn(220, 0, 0xFF); //AUTOISP_PROFILE_ADDR
			//Load Sensor Core Parameters
			writeCSRDefaults("Sensor_Core_Parameters");
			writeRegister(22, -1, 23, m_uiBrightness);

			wireIn(221, 0, 0xFF); //AUTOISP_BRT_EN, disable auto ISP
			wireIn(222, 0, 0xFF); //AUTOISP_TEM_EN
			wireIn(223, 0, 0xFF); //AUTOISP_TRIGGER

			writeRegister(225, -1, 224, m_uiAutoISPRefreshTime); //AUTOISP_REFRESH_TIME

			writeRegister(235, -1, 234, m_arrayISPThreshold[0]); //AUTOISP_BRT_THRES1
			writeRegister(237, -1, 236, m_arrayISPThreshold[1]); //AUTOISP_BRT_THRES2
			writeRegister(239, -1, 238, m_arrayISPThreshold[2]); //AUTOISP_BRT_THRES3

			writeRegister(233, -1, 232, 1500); //AUTOISP_BRT_VALUE
		}
		writeCSRDefaults("Sensor_Operation_Mode_Control_Parameters");

		writeCSRDefaults("Sensor_Data_Transfer_Parameters");
		wireIn(73, m_iEventDataFormat, 0xFF); //EVENT_PACKET_SELECT
		m_pDataProcessThread->getDataProcessor5()->setMIPIDataFormat(m_iEventDataFormat);

		cout << endl << "--- Enter Start Mode ---" << endl;
		enterStartMode();
	}
	return true;
}

bool CeleX5::resetConfigureSettings(DeviceType type)
{
	if (CeleX5::CeleX5_OpalKelly == type)
	{
		//--------------- Step1 ---------------
		wireIn(94, 1, 0xFF);

		//--------------- Step2 ---------------
		//Disable PLL
		wireIn(150, 0, 0xFF);
		//Load PLL Parameters
		writeCSRDefaults("PLL_Parameters");
		//Enable PLL
		wireIn(150, 1, 0xFF);

		//--------------- Step3 ---------------
		enterCFGMode();

		//Load Other Parameters
		wireIn(92, 12, 0xFF);
		wireIn(220, 0, 0xFF);
		//Load Sensor Core Parameters
		writeCSRDefaults("Sensor_Core_Parameters");

		wireIn(91, 11, 0xFF);
		wireIn(18, 151, 0xFF);

		wireIn(64, 0, 0xFF);
		//sensor mode
		wireIn(53, 2, 0xFF);
		wireIn(54, 2, 0xFF);
		wireIn(55, 2, 0xFF);

		wireIn(57, 60, 0xFF);
		wireIn(58, 0, 0xFF);

		enterStartMode();
	}
	else if (CeleX5::CeleX5_MIPI == type)
	{
		//cout << endl << "--- Enter CFG Mode ---" << endl;
		enterCFGMode();
#ifdef _WIN32
		Sleep(10);
#else
		usleep(1000 * 10);
#endif
		//Enter Start Mode
		//cout << endl << "--- Enter Start Mode ---" << endl;
		enterStartMode();
	}
	return true;
}

void CeleX5::wireIn(uint32_t address, uint32_t value, uint32_t mask)
{
	if (CeleX5::CeleX5_MIPI == m_emDeviceType)
	{
		if (m_pCeleDriver)
		{
			if (isAutoISPEnabled())
			{
				setALSEnabled(false);
#ifdef _WIN32
				Sleep(2);
#else
				usleep(1000 * 2);
#endif
			}
			if (m_pCeleDriver->i2c_set(address, value))
			{
				//cout << "CeleX5::wireIn(i2c_set): address = " << address << ", value = " << value << endl;
			}
			if (isAutoISPEnabled())
			{
				setALSEnabled(true);
			}
		}
	}
	else if (CeleX5::CeleX5_OpalKelly == m_emDeviceType)
	{
		//DAC_APPLY_OFF
		m_pFrontPanel->wireIn(0x05, 0x00, 0x02);
		//<command name = "set SPI_XXX to #value#">
		m_pFrontPanel->wireIn(0x04, value, 0x00FF); //0x03FF
													//<command name="set DAC_CHANNEL to #fixed value#">
		m_pFrontPanel->wireIn(0x04, (0x800 + address) << 8, 0xFFF00); //0xF000
		cout << "CeleX5::wireIn: address = " << address << ", value = " << value << endl;

		//DAC_APPLY_ON
		m_pFrontPanel->wireIn(0x05, 0x02, 0x02);

		//wire out the  data that have written  
		//uint32_t valueOut;
		//m_pFrontPanel->wireOut(0x20, 0xFFFF0000, &valueOut);
		//cout << hex << "wireout value = " << valueOut << endl;
	}
}

void CeleX5::writeRegister(CfgInfo cfgInfo)
{
	if (cfgInfo.low_addr == -1)
	{
		wireIn(cfgInfo.high_addr, cfgInfo.value, 0xFF);
	}
	else
	{
		if (cfgInfo.middle_addr == -1)
		{
			uint32_t valueH = cfgInfo.value >> 8;
			uint32_t valueL = 0xFF & cfgInfo.value;
			wireIn(cfgInfo.high_addr, valueH, 0xFF);
			wireIn(cfgInfo.low_addr, valueL, 0xFF);
		}
	}
}

void CeleX5::resetPin(bool bReset)
{
	uint32_t address = 0x00;
	uint32_t value = 0;
	uint32_t mask = 0x08;
	if (bReset)
	{
		value = 0x08;
	}
	m_pFrontPanel->wireIn(address, value, mask);
	cout << "CelexSensorDLL::resetPin: address = " << address << ", value = " << value << ", mask = " << mask << endl;
}

bool CeleX5::openBinFile(std::string filePath)
{
	m_uiPackageCount = 0;
	m_bFirstReadFinished = false;
	m_vecPackagePos.clear();
	if (m_ifstreamPlayback.is_open())
	{
		m_ifstreamPlayback.close();
	}
	m_pDataProcessThread->clearData();
	m_ifstreamPlayback.open(filePath.c_str(), std::ios::binary);
	if (!m_ifstreamPlayback.good())
	{
		cout << "Can't Open File: " << filePath.c_str() << endl;
		return false;
	}
	m_pDataProcessThread->setIsPlayback(true);
	m_pDataProcessThread->setPlaybackState(Playing);
	// read header
	m_ifstreamPlayback.read((char*)&m_stBinFileHeader, sizeof(BinFileAttributes));
	cout << "data_type = " << (int)m_stBinFileHeader.data_type
	  	 << ", loopA_mode = " << (int)m_stBinFileHeader.loopA_mode << ", loopB_mode = " << (int)m_stBinFileHeader.loopB_mode << ", loopC_mode = " << (int)m_stBinFileHeader.loopC_mode
		 << ", event_data_format = " << (int)m_stBinFileHeader.event_data_format
		 << ", hour = " << (int)m_stBinFileHeader.hour << ", minute = " << (int)m_stBinFileHeader.minute << ", second = " << (int)m_stBinFileHeader.second
		 << ", package_count = " << (int)m_stBinFileHeader.package_count << endl;

	m_uiTotalPackageCount = m_stBinFileHeader.package_count;

	m_pDataProcessThread->start();
	//if (m_emDeviceType == CeleX5::CeleX5_MIPI)
	{
		setEventDataFormat(m_stBinFileHeader.event_data_format);
	}
	if (m_stBinFileHeader.data_type == 1 || m_stBinFileHeader.data_type == 3)
	{
		m_bLoopModeEnabled = true;
		m_pDataProcessThread->getDataProcessor5()->setLoopModeEnabled(true);
	}
	else
	{
		m_bLoopModeEnabled = false;
		m_pDataProcessThread->getDataProcessor5()->setSensorFixedMode(CeleX5::CeleX5Mode(m_stBinFileHeader.loopA_mode));
	}
	return true;
}

bool CeleX5::readPlayBackData(long length)
{
	//cout << __FUNCTION__ << endl;
	bool eof = false;
	int maxLen = 128 * MAX_PAGE_COUNT;
	int lenToRead = length > maxLen ? maxLen : length;

	if (NULL == m_pReadBuffer)
		m_pReadBuffer = new unsigned char[maxLen];

	while (true && m_pDataProcessThread->queueSize() < 1000000)
	{
		m_ifstreamPlayback.read((char*)m_pReadBuffer, lenToRead);

		int dataLen = m_ifstreamPlayback.gcount();
		if (dataLen > 0)
			m_pDataProcessThread->addData(m_pReadBuffer, dataLen);

		if (m_ifstreamPlayback.eof())
		{
			eof = true;
			//m_ifstreamPlayback.close();
			cout << "Read Playback file Finished!" << endl;
			break;
		}
	}
	return eof;
}

bool CeleX5::readBinFileData()
{
	//cout << __FUNCTION__ << endl;
	if (m_pDataProcessThread->queueSize() > 10)
	{
		return false;
	}
	bool eof = false;

	uint64_t ifReadPos = m_ifstreamPlayback.tellg();

	uint32_t lenToRead = 0;
	m_ifstreamPlayback.read((char*)&lenToRead, 4);
	//cout << "lenToRead = " << lenToRead << endl;

	if (NULL == m_pDataToRead)
		m_pDataToRead = new uint8_t[2048000];
	m_ifstreamPlayback.read((char*)m_pDataToRead, lenToRead);	
	//
	int dataLen = m_ifstreamPlayback.gcount();
	if (dataLen > 0)
	{
		m_uiPackageCount++;
		if ((0x02 & m_stBinFileHeader.data_type) == 0x02) //has IMU data
		{
			time_t timeStamp;
			m_ifstreamPlayback.read((char*)&timeStamp, 8);
			m_pDataProcessThread->addData(m_pDataToRead, dataLen, timeStamp);

			int imuSize = 0;
			IMURawData imuRawData;
			vector<IMURawData> vecIMUData;
			m_ifstreamPlayback.read((char*)&imuSize, 4);
			if (imuSize > 0)
			{
				for (int i = 0; i < imuSize; i++)
				{
					m_ifstreamPlayback.read((char*)&imuRawData, sizeof(IMURawData));
					//cout << "imuRawData.time_stamp: "<<imuRawData.time_stamp << endl;
					vecIMUData.push_back(imuRawData);
				}
				m_pDataProcessThread->addIMUData(vecIMUData);
			}
		}
		else
		{
			m_pDataProcessThread->addData(m_pDataToRead, dataLen, 0);
		}
		if (!m_bFirstReadFinished)
		{
			m_vecPackagePos.push_back(ifReadPos);
			//cout << "--------------- " << m_vecPackagePos[m_uiPackageCount-1] << endl;
		}
		//cout << "package_count = " << m_uiPackageCount << endl;
	}
	if (m_ifstreamPlayback.eof())
	{
		eof = true;
		m_bFirstReadFinished = true;
		//m_ifstreamPlayback.close();
		setPlaybackState(BinReadFinished);
		cout << "Read Playback file Finished!" << endl;

		m_uiTotalPackageCount = m_uiPackageCount;
	}
	return eof;
}

uint32_t CeleX5::getTotalPackageCount()
{
	return m_uiTotalPackageCount;
}

uint32_t CeleX5::getCurrentPackageNo()
{
	//cout << "getCurrentPackageNo: " << m_pDataProcessThread->getPackageNo() << endl;
	return m_pDataProcessThread->getPackageNo();
}

void CeleX5::setCurrentPackageNo(uint32_t value)
{
	setPlaybackState(Replay);
	m_uiPackageCount = value;
	m_ifstreamPlayback.clear();
	m_ifstreamPlayback.seekg(m_vecPackagePos[value], ios::beg);
	m_pDataProcessThread->setPackageNo(value);
}

CeleX5::BinFileAttributes CeleX5::getBinFileAttributes()
{
	return m_stBinFileHeader;
}

void CeleX5::replay()
{
	setPlaybackState(Replay);
	m_uiPackageCount = 0;
	m_ifstreamPlayback.clear();
	m_ifstreamPlayback.seekg(sizeof(BinFileAttributes), ios::beg);
	m_pDataProcessThread->setPackageNo(0);
}

void CeleX5::play()
{
	m_pDataProcessThread->resume();
}

void CeleX5::pause()
{
	m_pDataProcessThread->suspend();
}

PlaybackState CeleX5::getPlaybackState()
{
	return m_pDataProcessThread->getPlaybackState();
}

void CeleX5::setPlaybackState(PlaybackState state)
{
	m_pDataProcessThread->setPlaybackState(state);
}

void CeleX5::setIsPlayBack(bool state)
{
	m_pDataProcessThread->setIsPlayback(state);
	if (!state) //stop playback
	{
		if (m_ifstreamPlayback.is_open())
		{
			m_ifstreamPlayback.close();
		}
	}
}

// FLICKER_DETECT_EN: CSR_183
// Flicker detection enable select: 1:enable / 0:disable
void CeleX5::setAntiFlashlightEnabled(bool enabled)
{
	enterCFGMode();
	if (enabled)
		writeRegister(183, -1, -1, 1);
	else
		writeRegister(183, -1, -1, 0);
	enterStartMode();
}

void CeleX5::setAutoISPEnabled(bool enable)
{
	m_bAutoISPEnabled = enable;
	if (enable)
	{
		enterCFGMode();

		wireIn(221, 1, 0xFF); //AUTOISP_BRT_EN, enable auto ISP
		if (isLoopModeEnabled())
			wireIn(223, 1, 0xFF); //AUTOISP_TRIGGER
		else
			wireIn(223, 0, 0xFF); //AUTOISP_TRIGGER

		wireIn(220, 0, 0xFF); //AUTOISP_PROFILE_ADDR, Write core parameters to profile0
		writeRegister(233, -1, 232, 1500); //AUTOISP_BRT_VALUE, Set initial brightness value 1500
		writeRegister(22, -1, 23, 80); //BIAS_BRT_I, Override the brightness value in profile0, avoid conflict with AUTOISP profile0		

		enterStartMode();

		setALSEnabled(true);
	}
	else
	{
		setALSEnabled(false); //Disable ALS read and write

		enterCFGMode();

		//Disable brightness adjustment (auto isp), always load sensor core parameters from profile0
		wireIn(221, 0, 0xFF); //AUTOISP_BRT_EN, disable auto ISP
		if (isLoopModeEnabled())
			wireIn(223, 1, 0xFF); //AUTOISP_TRIGGER
		else
			wireIn(223, 0, 0xFF); //AUTOISP_TRIGGER

		wireIn(220, 0, 0xFF); //AUTOISP_PROFILE_ADDR, Write core parameters to profile0
		writeRegister(233, -1, 232, 1500); //AUTOISP_BRT_VALUE, Set initial brightness value 1500
		writeRegister(22, -1, 23, 140); //BIAS_BRT_I, Override the brightness value in profile0, avoid conflict with AUTOISP profile0	
		enterStartMode();
	}
}

bool CeleX5::isAutoISPEnabled()
{
	return m_bAutoISPEnabled;
}

void CeleX5::setALSEnabled(bool enable)
{
	if (enable)
		m_pCeleDriver->i2c_set(254, 0);
	else
		m_pCeleDriver->i2c_set(254, 2);
}

void CeleX5::setISPThreshold(uint32_t value, int num)
{
	m_arrayISPThreshold[num - 1] = value;
	if (num == 1)
		writeRegister(235, -1, 234, m_arrayISPThreshold[0]); //AUTOISP_BRT_THRES1
	else if (num == 2)
		writeRegister(237, -1, 236, m_arrayISPThreshold[1]); //AUTOISP_BRT_THRES2
	else if (num == 3)
		writeRegister(239, -1, 238, m_arrayISPThreshold[2]); //AUTOISP_BRT_THRES3
}

void CeleX5::setISPBrightness(uint32_t value, int num)
{
	m_arrayBrightness[num - 1] = value;
	wireIn(220, num - 1, 0xFF); //AUTOISP_PROFILE_ADDR
	writeRegister(22, -1, 23, m_arrayBrightness[num - 1]);
}

void CeleX5::setRowDisabled(uint8_t rowMask)
{
	enterCFGMode();
	wireIn(44, rowMask, 0xFF);
	enterStartMode();
}

void CeleX5::setShowImagesEnabled(bool enable)
{
	m_pDataProcessThread->setShowImagesEnabled(enable);
}

uint32_t CeleX5::getISOLevelCount()
{
	return m_uiISOLevelCount;
}

int CeleX5::getIMUData(std::vector<IMUData>& data)
{
	return m_pDataProcessThread->getDataProcessor5()->getIMUData(data);
}

//Enter CFG Mode
void CeleX5::enterCFGMode()
{
	wireIn(93, 0, 0xFF);
	wireIn(90, 1, 0xFF);
}

//Enter Start Mode
void CeleX5::enterStartMode()
{
	wireIn(90, 0, 0xFF);
	wireIn(93, 1, 0xFF);
}

void CeleX5::disableMIPI()
{
	wireIn(139, 0, 0xFF);
	wireIn(140, 0, 0xFF);
	wireIn(141, 0, 0xFF);
	wireIn(142, 0, 0xFF);
	wireIn(143, 0, 0xFF);
	wireIn(144, 0, 0xFF);
}

void CeleX5::enableMIPI()
{
	wireIn(139, 1, 0xFF);
	wireIn(140, 1, 0xFF);
	wireIn(141, 1, 0xFF);
	wireIn(142, 1, 0xFF);
	wireIn(143, 1, 0xFF);
	wireIn(144, 1, 0xFF);
}

void CeleX5::clearData()
{
	m_pDataProcessThread->clearData();
	if (CeleX5::CeleX5_MIPI == m_emDeviceType)
	{
		m_pCeleDriver->clearData();
	}
}

int CeleX5::denoisingMaskByEventTime(const cv::Mat& countEventImg, double timelength, cv::Mat& denoiseMaskImg)
{
	if (countEventImg.empty())
	{
		return 0;
	}
	//cv::Mat kern = (cv::Mat_<uchar>(3, 3) << 0, 1, 0, 1, 1, 1, 0, 1, 0);//4-neighbor for convolution
	//cv::Mat convimg;
	//countEventImg.convertTo(convimg, CV_32FC1);
	//cv::filter2D(convimg, convimg, convimg.depth(), kern);

	//int timeslicecount = timelength / 800;//time step for density estimation, assigned by experience
	//int thresh = timeslicecount * 3;
	//cv::threshold(convimg, denoiseMaskImg, thresh, 255, CV_THRESH_BINARY);
	//denoiseMaskImg.convertTo(denoiseMaskImg, CV_8UC1);
	//return 1;

	cv::Mat kern = (cv::Mat_<uchar>(3, 3) << 1, 1, 1, 1, 1, 1, 1, 1, 1);
	//cv::Mat kern = (cv::Mat_<uchar>(3, 3) << 0, 1, 0, 1, 1, 1, 0, 1, 0);
	cv::Mat convimg;
	cv::filter2D(countEventImg, convimg, countEventImg.depth(), kern);

	int timeslicecount = timelength / 30.0;//time step for density estimation, assigned by experience
	int thresh = timeslicecount * 5;
	cv::threshold(convimg, denoiseMaskImg, thresh, 255, CV_THRESH_BINARY);
	denoiseMaskImg.convertTo(denoiseMaskImg, CV_8UC1);
	return 1;
}

void CeleX5::saveFullPicRawData()
{
	m_pDataProcessThread->getDataProcessor5()->saveFullPicRawData();
}
