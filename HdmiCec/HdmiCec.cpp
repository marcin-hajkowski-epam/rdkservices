/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2019 RDK Management
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#include <mutex>

#include "HdmiCec.h"


#include "ccec/Connection.hpp"
#include "ccec/CECFrame.hpp"
#include "host.hpp"
#include "ccec/host/RDK.hpp"

#include "ccec/drivers/iarmbus/CecIARMBusMgr.h"


#include "dsMgr.h"
#include "dsDisplay.h"
#include "videoOutputPort.hpp"
#include "manager.hpp"

#include "websocket/URL.h"

#include "utils.h"

#define HDMICEC_METHOD_SET_ENABLED "setEnabled"
#define HDMICEC_METHOD_GET_ENABLED "getEnabled"
#define HDMICEC_METHOD_GET_CEC_ADDRESSES "getCECAddresses"
#define HDMICEC_METHOD_SEND_MESSAGE "sendMessage"
#define HDMICEC_METHOD_ENABLE_ONE_TOUCH_VIEW "enableOneTouchView"
#define HDMICEC_METHOD_TRIGGER_ACTION "triggerAction"
#define HDMICEC_METHOD_SET_PING_INTERVAL "setPingInterval"
#define HDMICEC_METHOD_GET_CONNECTED_DEVICES "getConnectedDevices"
#define HDMICEC_METHOD_SET_NAME "setName"

#define HDMICEC_EVENT_ON_DEVICES_CHANGED "onDevicesChanged"
#define HDMICEC_EVENT_ON_MESSAGE "onMessage"
#define HDMICEC_EVENT_ON_HDMI_HOT_PLUG "onHdmiHotPlug"
#define HDMICEC_EVENT_ON_CEC_ADDRESS_CHANGE "cecAddressesChanged"
#define HDMICEC_METHOD_SET_ONE_TOUCH_VIEW_POLICY "setOneTouchViewPolicy"

#define PHYSICAL_ADDR_CHANGED 1
#define LOGICAL_ADDR_CHANGED 2
#define DEV_TYPE_TUNER 1

#define HDMI_HOT_PLUG_EVENT_CONNECTED 0
#define HDMI_HOT_PLUG_EVENT_DISCONNECTED 1

#if defined(HAS_PERSISTENT_IN_HDD)
#define CEC_SETTING_ENABLED_FILE "/tmp/mnt/diska3/persistent/ds/cecData.json"
#elif defined(HAS_PERSISTENT_IN_FLASH)
#define CEC_SETTING_ENABLED_FILE "/opt/persistent/ds/cecData.json"
#else
#define CEC_SETTING_ENABLED_FILE "/opt/ds/cecData.json"
#endif

#define CEC_SETTING_ENABLED "cecEnabled"

namespace
{
    using namespace WPEFramework;

    inline void requestRescanning(int id)
    {
        IARM_Bus_CECHost_ConfigureScan_Param_t param  {};
        param.needFullUpdate = true;
        param.scanReasonId = id;

        const IARM_Result_t ret = IARM_Bus_Call(IARM_BUS_CECHOST_NAME,
                                                IARM_BUS_CEC_HOST_ConfigureScan,
                                                (void *)&param,sizeof(param));

        if (IARM_RESULT_SUCCESS != ret)
        {
            LOGERR("%s failed result %d", IARM_BUS_CEC_HOST_ConfigureScan, ret);
        }
    }
    inline void setOsdName(const string& name)
    {
        IARM_Bus_CECHost_SetOSDName_Param_t param {};

        strncpy((char *)param.name, name.c_str(), sizeof(param.name));
        param.name[sizeof(param.name) - 1] = '\0';
        const IARM_Result_t ret = IARM_Bus_Call(IARM_BUS_CECHOST_NAME,
                                                IARM_BUS_CEC_HOST_SetOSDName,
                                                (void *)&param,
                                                sizeof(param));
        if (IARM_RESULT_SUCCESS != ret)
        {
            LOGERR("%s failed result %d", IARM_BUS_CEC_HOST_SetOSDName, ret);
        }
    }
}

namespace WPEFramework
{
    namespace Plugin
    {
        SERVICE_REGISTRATION(HdmiCec, 1, 0);

        HdmiCec* HdmiCec::_instance = nullptr;

        static int libcecInitStatus = 0;

        HdmiCec::HdmiCec()
        : AbstractPlugin(),cecEnableStatus(false),smConnection(nullptr),
            m_scan_id(0), m_updated(false), m_rescan_in_progress(true), m_system_audio_mode(false)
        {
            HdmiCec::_instance = this;

            InitializeIARM();
            device::Manager::Initialize();

            registerMethod(HDMICEC_METHOD_SET_ENABLED, &HdmiCec::setEnabledWrapper, this);
            registerMethod(HDMICEC_METHOD_GET_ENABLED, &HdmiCec::getEnabledWrapper, this);
            registerMethod(HDMICEC_METHOD_GET_CEC_ADDRESSES, &HdmiCec::getCECAddressesWrapper, this);
            registerMethod(HDMICEC_METHOD_SEND_MESSAGE, &HdmiCec::sendMessageWrapper, this);
            registerMethod(HDMICEC_METHOD_ENABLE_ONE_TOUCH_VIEW, &HdmiCec::enableOneTouchViewWrapper, this);
            registerMethod(HDMICEC_METHOD_TRIGGER_ACTION, &HdmiCec::triggerActionWrapper, this);
            registerMethod(HDMICEC_METHOD_SET_PING_INTERVAL, &HdmiCec::setPingIntervalWrapper, this);
            registerMethod(HDMICEC_METHOD_GET_CONNECTED_DEVICES, &HdmiCec::getConnectedDevicesWrapper, this);
            registerMethod(HDMICEC_METHOD_SET_NAME, &HdmiCec::setNameWrapper, this);
            registerMethod(HDMICEC_METHOD_SET_ONE_TOUCH_VIEW_POLICY, &HdmiCec::setOneTouchViewPolicyWrapper, this);

            physicalAddress = 0x0F0F0F0F;

            logicalAddressDeviceType = "None";
            logicalAddress = 0xFF;

            loadSettings();
            if (cecSettingEnabled)
            {
                setEnabled(cecSettingEnabled);
            }
            else
            {
                setEnabled(false);
                persistSettings(false);
            }

            m_scan_id++;
            requestRescanning(m_scan_id);
        }

        HdmiCec::~HdmiCec()
        {
        }

        void HdmiCec::Deinitialize(PluginHost::IShell* /* service */)
        {
            HdmiCec::_instance = nullptr;

            DeinitializeIARM();

        }

        const void HdmiCec::InitializeIARM()
        {
            if (Utils::IARM::init())
            {
                IARM_Result_t res;
                IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_DAEMON_INITIALIZED,cecMgrEventHandler) );
                IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_STATUS_UPDATED,cecMgrEventHandler) );
                IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG, dsHdmiEventHandler) );
                IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECHOST_NAME, IARM_BUS_CECHost_EVENT_DEVICESTATUSCHANGE, cecHostDeviceStatusChangedEventHandler) );
                IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECHOST_NAME, IARM_BUS_CECHost_EVENT_DEVICESTATUSUPDATEEND, cecHostDeviceStatusUpdateEndEventHandler) );
            }
        }

        void HdmiCec::DeinitializeIARM()
        {
            if (Utils::IARM::isConnected())
            {
                IARM_Result_t res;
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_DAEMON_INITIALIZED) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_STATUS_UPDATED) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECHOST_NAME, IARM_BUS_CECHost_EVENT_DEVICESTATUSCHANGE) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECHOST_NAME, IARM_BUS_CECHost_EVENT_DEVICESTATUSUPDATEEND) );
            }
        }

        void HdmiCec::cecMgrEventHandler(const char *owner, IARM_EventId_t eventId, void *data, size_t len)
        {
            if(!HdmiCec::_instance)
                return;

            if( !strcmp(owner, IARM_BUS_CECMGR_NAME))
            {
                switch (eventId)
                {
                    case IARM_BUS_CECMGR_EVENT_DAEMON_INITIALIZED:
                    {
                        HdmiCec::_instance->onCECDaemonInit();
                    }
                    break;
                    case IARM_BUS_CECMGR_EVENT_STATUS_UPDATED:
                    {
                        IARM_Bus_CECMgr_Status_Updated_Param_t *evtData = new IARM_Bus_CECMgr_Status_Updated_Param_t;
                        if(evtData)
                        {
                            memcpy(evtData,data,sizeof(IARM_Bus_CECMgr_Status_Updated_Param_t));
                            HdmiCec::_instance->cecStatusUpdated(evtData);
                        }
                    }
                    break;
                    default:
                    /*Do nothing*/
                    break;
                }
            }
        }

        void HdmiCec::dsHdmiEventHandler(const char *owner, IARM_EventId_t eventId, void *data, size_t len)
        {
            if(!HdmiCec::_instance)
                return;

            if (IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG == eventId)
            {
                IARM_Bus_DSMgr_EventData_t *eventData = (IARM_Bus_DSMgr_EventData_t *)data;
                int hdmi_hotplug_event = eventData->data.hdmi_hpd.event;
                LOGINFO("Received IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG  event data:%d \r\n", hdmi_hotplug_event);

                HdmiCec::_instance->onHdmiHotPlug(hdmi_hotplug_event);
            }
        }

        void HdmiCec::onCECDaemonInit()
        {
            if(true == getEnabled())
            {
                setEnabled(false);
                setEnabled(true);
            }
            else
            {
                /*Do nothing as CEC is not already enabled*/
            }
        }

        void HdmiCec::cecStatusUpdated(void *evtStatus)
        {
            IARM_Bus_CECMgr_Status_Updated_Param_t *evtData = (IARM_Bus_CECMgr_Status_Updated_Param_t *)evtStatus;
            if(evtData)
            {
               try{
                    getPhysicalAddress();

                    unsigned int logicalAddr = evtData->logicalAddress;
                    std::string logicalAddrDeviceType = DeviceType(LogicalAddress(evtData->logicalAddress).getType()).toString().c_str();

                    LOGWARN("cecLogicalAddressUpdated: logical address updated: %d , saved : %d ", logicalAddr, logicalAddress);
                    if (logicalAddr != logicalAddress || logicalAddrDeviceType != logicalAddressDeviceType)
                    {
                        logicalAddress = logicalAddr;
                        logicalAddressDeviceType = logicalAddrDeviceType;
                        cecAddressesChanged(LOGICAL_ADDR_CHANGED);
                    }
                }
                catch (const std::exception& e)
                {
                    LOGWARN("CEC exception caught from cecStatusUpdated");
                }

                delete evtData;
            }
           return;
        }

        void HdmiCec::onHdmiHotPlug(int connectStatus)
        {
            LOGINFO("Status %d", connectStatus);
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                m_devices.clear();
                m_scan_devices.clear();
                m_rescan_in_progress = true;
            }
            if ((HDMI_HOT_PLUG_EVENT_CONNECTED == connectStatus) && cecEnableStatus)
            {
                try
                {
                    readAddresses();
                }
                catch (const std::exception& e)  {
                    LOGWARN("CEC addresses not present");
                }
                m_scan_id++;
                requestRescanning(m_scan_id);
            }
            if (HDMI_HOT_PLUG_EVENT_DISCONNECTED == connectStatus)
            {
                physicalAddress = 0x0F0F0F0F;
                logicalAddress = 0xFF;
                logicalAddressDeviceType = "None";
                cecAddressesChanged(PHYSICAL_ADDR_CHANGED);
                //avoid race - we can receive update just after hotplug
                // and before re-scan
                m_updated = false;

                onDevicesChanged();
            }
            return;
        }

        uint32_t HdmiCec::setEnabledWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFOMETHOD();

            const char *parameterName = "enabled";
            returnIfBooleanParamNotFound(parameters, parameterName);
            bool enabled = false;
            getBoolParameter(parameterName, enabled);

            setEnabled(enabled);
            returnResponse(true);
        }

        uint32_t HdmiCec::getEnabledWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFOMETHOD();

            response["enabled"] = getEnabled();
            returnResponse(true);
        }

        uint32_t HdmiCec::getCECAddressesWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFOMETHOD();

            response["CECAddresses"] = getCECAddresses();

            returnResponse(true);
        }

        uint32_t HdmiCec::sendMessageWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFOMETHOD();

            std::string message;

            if (parameters.HasLabel("message"))
            {
                message = parameters["message"].String();
            }
            else
            {
                returnResponse(false);
            }

            sendMessage(message);
            returnResponse(true);
        }

        uint32_t HdmiCec::enableOneTouchViewWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            const char *parameterName = "enabled";
            returnIfBooleanParamNotFound(parameters, parameterName);

            IARM_Bus_CECHost_EnableOneTouchView_Param_t param;
            getBoolParameter(parameterName, param.enableOneTouchView);

            const IARM_Result_t ret = IARM_Bus_Call(IARM_BUS_CECHOST_NAME,
                                                    (char *)IARM_BUS_CEC_HOST_EnableOneTouchView,
                                                    (void *)&param, sizeof(param));
            if (ret != IARM_RESULT_SUCCESS)
            {
                LOGERR("Enabling one touch view failed.");
                returnResponse(false);
            }

            LOGINFO("Successfully %s one touch view.",
                    param.enableOneTouchView ? "enabled": "disabled");
            returnResponse(true);
        }

        uint32_t HdmiCec::triggerActionWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            const char *parameterName = "actionName";
            returnIfStringParamNotFound(parameters, parameterName);

            string actionName;
            getStringParameter(parameterName, actionName);

            LOGINFO("CEC: %s Triggering action[%s]\n", __FUNCTION__, actionName.c_str());

            IARM_Bus_CECHost_TriggerAction_Param_t param;
            strncpy(param.name,
                    actionName.c_str(),
                    sizeof(param.name));
            param.name[CEC_ACTION_NAME_LEN - 1] = '\0';
            param.destination = 0x0F;

            const IARM_Result_t ret = IARM_Bus_Call(IARM_BUS_CECHOST_NAME,
                                                    IARM_BUS_CEC_HOST_TriggerAction,
                                                    static_cast<void*>(&param), sizeof(param));

            if (IARM_RESULT_SUCCESS != ret)
            {
                LOGERR("CEC: ERROR - %s CALL[%s], ACTION[%s] failed result %d\n",
                       __FUNCTION__,
                       IARM_BUS_CEC_HOST_TriggerAction,
                       actionName.c_str(),
                       ret);
                returnResponse(false);
            }

            LOGINFO("CEC: SUCCESS - %s CALL[%s], ACTION[%s]\n", __FUNCTION__, IARM_BUS_CEC_HOST_TriggerAction,
                    actionName.c_str());

            returnResponse(true);
        }

        uint32_t HdmiCec::setPingIntervalWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            const char *parameterName = "intervalSeconds";

            returnIfNumberParamNotFound(parameters, parameterName);

            IARM_Bus_CECHost_SetScanInterval_Param_t param;
            getNumberParameter(parameterName, param.intervalSeconds);

            const IARM_Result_t ret = IARM_Bus_Call(IARM_BUS_CECHOST_NAME,
                                                    IARM_BUS_CEC_HOST_SetScanInterval,
                                                    static_cast<void*>(&param),
                                                    sizeof(param));
            if (IARM_RESULT_SUCCESS != ret)
            {
                LOGERR("CEC: ERROR - %s %s failed result %d\n",
                       __FUNCTION__, IARM_BUS_CEC_HOST_SetScanInterval, ret);
                returnResponse(false);
            }
            LOGINFO("CEC: %s  interval %d succeed\n", __FUNCTION__, param.intervalSeconds);

            returnResponse(true);
        }

        void HdmiCec::onDevicesChanged()
        {
            LOGINFO();

            JsonArray deviceList;
            getConnectedDevices(deviceList);
            JsonObject parameters;
            parameters["devices"] = deviceList;
            sendNotify(HDMICEC_EVENT_ON_DEVICES_CHANGED, parameters);
        }

        void HdmiCec::getConnectedDevices(JsonArray &deviceList)
        {
            LOGINFO();
            bool connected = false;
            try
            {
                device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
                connected = vPort.isDisplayConnected();
            }
            catch (...)
            {
                LOGWARN("Checking HDMI0 display connection state failed");
            }

            if (!connected)
            {
                LOGINFO("HDMI disconnected - empty devices list");

                return;
            }

            try
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                for (m_devices_map_t::iterator itr = m_devices.begin(); itr != m_devices.end();++itr)
                {
                    JsonObject device;
                    device["vendorId"] = (*itr).second.vendor_id;
                    device["osdName"] =  (*itr).second.osdName;
                    device["power"] = (*itr).second.power_state;
                    device["connected"] =  (*itr).second.connected;
                    device["device"] = (*itr).first;
                    deviceList.Add(device);
                    LOGINFO("CEC: added device: vendorid '%s' name '%s' power %d conn %d dev '%s'\n",
                            (*itr).second.vendor_id.c_str(),
                            (*itr).second.osdName.c_str(),
                            static_cast<int>((*itr).second.power_state),
                            static_cast<int>((*itr).second.connected),
                            (*itr).first.c_str());
                }
            }
            catch (const std::exception& e)  {
                LOGWARN("failed");
            }
        }

        uint32_t HdmiCec::getConnectedDevicesWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            JsonArray deviceList;

            if(cecEnableStatus == true)
            {
                getConnectedDevices(deviceList);
                // force sending onDeviceChanged event on next scan end
                m_updated = (m_updated || (deviceList.IsNull() && m_rescan_in_progress));
            }
            else
            {
                LOGERR("CEC: %s failed - CEC disabled\n", __FUNCTION__);
            }
            response["devices"] = deviceList;
            response["systemAudioMode"] = static_cast<bool>(m_system_audio_mode);

            returnResponse(deviceList.IsSet() || (m_rescan_in_progress == false));
        }
 
        uint32_t HdmiCec::setNameWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            string name;

            returnIfStringParamNotFound(parameters, "name");
            getStringParameter("name", name);

            LOGINFO("%s", name.c_str());

            try
            {
                setOsdName(name);
            }
            catch (const std::exception& e)
            {
                LOGWARN("failed");
                returnResponse(false);
            }
            returnResponse(true);
        }

        uint32_t HdmiCec::setOneTouchViewPolicyWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            returnIfBooleanParamNotFound(parameters, "turnOffAllDevices");
            bool turnOffAllDevices = true;
            getBoolParameter("turnOffAllDevices", turnOffAllDevices);

            LOGINFO("turnOffDevices[%s]\n", turnOffAllDevices?"TRUE":"FALSE");

            IARM_Bus_CECHost_SetOneTouchViewPolicy_Param_t param;
            param.turnOffAllDevices = turnOffAllDevices;
            const IARM_Result_t ret = IARM_Bus_Call(IARM_BUS_CECHOST_NAME,
                                                    IARM_BUS_CEC_HOST_SetOneTouchViewPolicy,
                                                    static_cast<void*>(&param),
                                                    sizeof(param));

            if (IARM_RESULT_SUCCESS != ret)
            {
                LOGERR("%s failed result %d\n",
                       IARM_BUS_CEC_HOST_SetOneTouchViewPolicy,
                       ret);
                returnResponse(false);
            }

            LOGINFO("SUCCESS - %s\n", IARM_BUS_CEC_HOST_SetOneTouchViewPolicy);
            returnResponse(true);
        }

        bool HdmiCec::loadSettings()
        {
            Core::File file;
            file = CEC_SETTING_ENABLED_FILE;

            file.Open();
            JsonObject parameters;
            parameters.IElement::FromFile(file);

            file.Close();

            getBoolParameter(CEC_SETTING_ENABLED, cecSettingEnabled);

            return cecSettingEnabled;
        }

        void HdmiCec::persistSettings(bool enableStatus)
        {
            Core::File file;
            file = CEC_SETTING_ENABLED_FILE;

            file.Open(false);
            if (!file.IsOpen())
                file.Create();

            JsonObject cecSetting;
            cecSetting[CEC_SETTING_ENABLED] = enableStatus;

            cecSetting.IElement::ToFile(file);

            file.Close();

            return;
        }

        void HdmiCec::setEnabled(bool enabled)
        {
           LOGWARN("Entered setEnabled ");

           if (cecSettingEnabled != enabled)
           {
               persistSettings(enabled);
           }
           if(true == enabled)
           {
               CECEnable();
           }
           else
           {
               CECDisable();
           }
           return;
        }

        void HdmiCec::CECEnable(void)
        {
            LOGWARN("Entered CECEnable");
            if (cecEnableStatus)
            {
                LOGWARN("CEC Already Enabled");
                return;
            }

            if(0 == libcecInitStatus)
            {
                try
                {
                    LibCCEC::getInstance().init();
                }
                catch (const std::exception e)
                {
                    LOGWARN("CEC exception caught from CECEnable");
                }
            }
            libcecInitStatus++;

            smConnection = new Connection(LogicalAddress::UNREGISTERED,false,"ServiceManager::Connection::");
            smConnection->open();
            smConnection->addFrameListener(this);

            //Acquire CEC Addresses
            getPhysicalAddress();
            getLogicalAddress();

            cecEnableStatus = true;
            return;
        }

        void HdmiCec::CECDisable(void)
        {
            LOGWARN("Entered CECDisable ");

            if(!cecEnableStatus)
            {
                LOGWARN("CEC Already Disabled ");
                return;
            }

            if (smConnection != NULL)
            {
                smConnection->close();
                delete smConnection;
                smConnection = NULL;
            }
            cecEnableStatus = false;

            if(1 == libcecInitStatus)
            {
                LibCCEC::getInstance().term();
            }

            if(libcecInitStatus > 0)
            {
                libcecInitStatus--;
            }

            return;
        }

        void HdmiCec::readAddresses()
        {
            LOGINFO();

            try
            {
              device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
              if (vPort.isDisplayConnected())
              {
                  if (getPhysicalAddress() || getLogicalAddress())
                  {
                      cecAddressesChanged(PHYSICAL_ADDR_CHANGED);
                  }
              }
            }
            catch (const std::exception& e)
            {
                LOGWARN("exception caught from %s\r\n", __FUNCTION__);
                throw;
            }
        }

        bool HdmiCec::getPhysicalAddress()
        {
            bool changed = false;
            LOGINFO("Entered getPhysicalAddress ");

            uint32_t physAddress = 0x0F0F0F0F;

            try {
                    LibCCEC::getInstance().getPhysicalAddress(&physAddress);

                    LOGINFO("getPhysicalAddress: physicalAddress: %x %x %x %x ", (physAddress >> 24) & 0xFF, (physAddress >> 16) & 0xFF, (physAddress >> 8)  & 0xFF, (physAddress) & 0xFF);
                    if (physAddress != physicalAddress)
                    {
                        physicalAddress = physAddress;
                        changed = true;
                    }
            }
            catch (const std::exception& e)
            {
                LOGWARN("DS exception caught from getPhysicalAddress");
                throw;
            }
            return changed;
        }

        bool HdmiCec::getLogicalAddress()
        {
            LOGINFO("Entered getLogicalAddress ");
            bool changed = false;

            try{
                int addr = LibCCEC::getInstance().getLogicalAddress(DEV_TYPE_TUNER);

                std::string logicalAddrDeviceType = DeviceType(LogicalAddress(addr).getType()).toString().c_str();

                LOGWARN("logical address obtained is %d , saved logical address is %d ", addr, logicalAddress);

                if ((int)logicalAddress != addr || logicalAddressDeviceType != logicalAddrDeviceType)

                {
                    logicalAddress = addr;
                    logicalAddressDeviceType = logicalAddrDeviceType;
                    changed = true;
                }
            }
            catch (const std::exception& e)
            {
                LOGWARN("CEC exception caught from getLogicalAddress ");
                throw;
            }

            return changed;
        }

        bool HdmiCec::getEnabled()
        {
            LOGWARN("Entered getEnabled ");
            if(true == cecEnableStatus)
                return true;
            else
                return false;
        }

        std::string HdmiCec::getName()
        {
            //SVCLOG_WARN("%s \r\n",__FUNCTION__);
            IARM_Result_t ret = IARM_RESULT_INVALID_STATE;
            if (ret != IARM_RESULT_SUCCESS)
            {
                LOGWARN("getName :: IARM_BUS_CEC_HOST_GetOSDName failed ");
                return "STB";
            }

            return "STB";
        }

        JsonObject HdmiCec::getCECAddresses()
        {
            JsonObject CECAddress;
            LOGINFO("Entered getCECAddresses ");

            char pa[32] = {0};
            snprintf(pa, sizeof(pa), "\\u00%02X\\u00%02X\\u00%02X\\u00%02X", (physicalAddress >> 24) & 0xff, (physicalAddress >> 16) & 0xff, (physicalAddress >> 8) & 0xff, physicalAddress & 0xff);

            CECAddress["physicalAddress"] = (const char *)pa;

            JsonObject logical;
            logical["deviceType"] = logicalAddressDeviceType;
            logical["logicalAddress"] = logicalAddress;

            CECAddress["logicalAddresses"] = logical;
            LOGWARN("getCECAddresses: physicalAddress from QByteArray : %x %x %x %x ", (physicalAddress >> 24) & 0xFF, (physicalAddress >> 16) & 0xFF, (physicalAddress >> 8)  & 0xFF, (physicalAddress) & 0xFF);
            LOGWARN("getCECAddresses: logical address: %x  ", logicalAddress);

            return CECAddress;
        }

        // Copy of Core::FromString, which doesn't add extra zero at the end
        uint16_t HdmiCec::FromBase64String(const string& newValue, uint8_t object[], uint16_t& length, const TCHAR* ignoreList)
        {
            uint8_t state = 0;
            uint16_t index = 0;
            uint16_t filler = 0;
            uint8_t lastStuff = 0;

            while ((index < newValue.size()) && (filler < length)) {
                uint8_t converted;
                TCHAR current = newValue[index++];

                if ((current >= 'A') && (current <= 'Z')) {
                    converted = (current - 'A');
                } else if ((current >= 'a') && (current <= 'z')) {
                    converted = (current - 'a' + 26);
                } else if ((current >= '0') && (current <= '9')) {
                    converted = (current - '0' + 52);
                } else if (current == '+') {
                    converted = 62;
                } else if (current == '/') {
                    converted = 63;
                } else if ((ignoreList != nullptr) && (::strchr(ignoreList, current) != nullptr)) {
                    continue;
                } else {
                    break;
                }

                if (state == 0) {
                    lastStuff = converted << 2;
                    state = 1;
                } else if (state == 1) {
                    object[filler++] = (((converted & 0x30) >> 4) | lastStuff);
                    lastStuff = ((converted & 0x0F) << 4);
                    state = 2;
                } else if (state == 2) {
                    object[filler++] = (((converted & 0x3C) >> 2) | lastStuff);
                    lastStuff = ((converted & 0x03) << 6);
                    state = 3;
                } else if (state == 3) {
                    object[filler++] = ((converted & 0x3F) | lastStuff);
                    state = 0;
                }
            }

            // No need to do this
            /*if ((state != 0) && (filler < length)) {
                object[filler++] = lastStuff;
                LOGINFO("state %d, lastStuff = %d", state, lastStuff);
            }*/

            length = filler;

            return (index);
        }

        void HdmiCec::sendMessage(std::string message)
        {
            LOGINFO("sendMessage ");

            if(true == cecEnableStatus)
            {
                std::vector <unsigned char> buf;
                buf.resize(message.size());

                uint16_t decodedLen = message.size();
                FromBase64String(message, (uint8_t*)buf.data(), decodedLen, NULL);

                CECFrame frame = CECFrame((const uint8_t *)buf.data(), decodedLen);
        //      SVCLOG_WARN("Frame to be sent from servicemanager in %s \n",__FUNCTION__);
        //      frame.hexDump();
                smConnection->sendAsync(frame);
            }
            else
                LOGWARN("cecEnableStatus=false");
            return;
        }

        void HdmiCec::cecAddressesChanged(int changeStatus)
        {
            JsonObject params;
            JsonObject CECAddresses;

            LOGWARN(" cecAddressesChanged Change Status : %d ", changeStatus);
            if(PHYSICAL_ADDR_CHANGED == changeStatus)
            {
                char pa[32] = {0};
                snprintf(pa, sizeof(pa), "\\u00%02X\\u00%02X\\u00%02X\\u00%02X", (physicalAddress >> 24) & 0xff, (physicalAddress >> 16) & 0xff, (physicalAddress >> 8) & 0xff, physicalAddress & 0xff);

                CECAddresses["physicalAddress"] = (const char *)pa;
            }
            else if(LOGICAL_ADDR_CHANGED == changeStatus)
            {
                CECAddresses["logicalAddresses"] = logicalAddress;
            }
            else
            {
                //Do Nothing
            }

            params["CECAddresses"] = CECAddresses;
            LOGWARN(" cecAddressesChanged  send : %s ", HDMICEC_EVENT_ON_CEC_ADDRESS_CHANGE);

            sendNotify(HDMICEC_EVENT_ON_CEC_ADDRESS_CHANGE, params);

            return;
        }

        void HdmiCec::notify(const CECFrame &in) const
        {
            LOGINFO("Inside notify ");
            size_t length;
            const uint8_t *input_frameBuf = NULL;
            CECFrame Frame = in;
        //  SVCLOG_WARN("Frame received by servicemanager is \n");
        //  Frame.hexDump();
            Frame.getBuffer(&input_frameBuf,&length);

            std::vector <char> buf;
            buf.resize(length * 2);

            uint16_t encodedLen = Core::URL::Base64Encode(input_frameBuf, length, buf.data(), buf.size());
            buf[encodedLen] = 0;

            (const_cast<HdmiCec*>(this))->onMessage(buf.data());
            return;
        }

        void HdmiCec::onMessage( const char *message )
        {
            JsonObject params;
            params["message"] = message;
            sendNotify(HDMICEC_EVENT_ON_MESSAGE, params);
        }

        void HdmiCec::onDeviceStatusChanged(IARM_EventId_t eventId, const void* data_ptr, size_t len)
        {
            const IARM_Bus_CECHost_DeviceStatusChanged_EventData_t* eData = static_cast<const IARM_Bus_CECHost_DeviceStatusChanged_EventData_t*>(data_ptr);

            LOGINFO("id=%d, len=%u", (int)eventId, (unsigned)len);

            if ((eData->logicalAddress == LogicalAddress::TV) || (eData->logicalAddress == LogicalAddress::AUDIO_SYSTEM))
            { // the scanner may send events for other devices, so skip them ...
                bool update = false;
                try
                {
                    readAddresses();

                    switch(eData->changedStatus)
                    {
                      case IARM_BUS_CECHost_OSD_NAME:
                      {
                          LOGINFO("IARM_BUS_CECHost_OSD_NAME for device %d name %s", eData->logicalAddress, eData->data.osdName);
                          update = setOSDName(eData->data.osdName, eData->logicalAddress);
                          break;
                      }
                      case IARM_BUS_CECHost_VENDOR_ID:
                      {
                          LOGINFO("IARM_BUS_CECHost_VENDOR_ID for device %d vendor id 0x%x", eData->logicalAddress, eData->data.vendorId);
                          update = setVendorId(eData->data.vendorId, eData->logicalAddress);
                          break;
                      }
                      case IARM_BUS_CECHost_POWER_STATUS:
                      {
                          LOGINFO("IARM_BUS_CECHost_POWER_STATUS for device %d powerState %d", eData->logicalAddress, eData->data.powerState);
                          update = setPowerState(eData->data.powerState, eData->logicalAddress);
                          break;
                      }
                      case IARM_BUS_CECHost_CONNECT_STATUS:
                      {
                          LOGINFO("IARM_BUS_CECHost_CONNECT_STATUS for device %d isConnected %d", eData->logicalAddress, eData->data.isConnected);
                          update = setConnectedState(eData->data.isConnected, eData->logicalAddress);
                          break;
                      }
		      case IARM_BUS_CECHost_AUDIO_MODE:
		      {
			  LOGINFO("IARM_BUS_CECHost_AUDIO_MODE systemAudioMode %d\n", eData->data.systemAudioMode);
			  update = setSystemAudioMode(eData->data.systemAudioMode);
			  break;
		      }
                      default:
                      {
                          LOGWARN("Unsupported event IARM_BUS_CECHost %d !!!!!", eData->changedStatus);
                          break;
                      }
                    }
                }
                catch (const std::exception& e)
                {
                    LOGERR("exception caught");
                }
                if (update)
                {
                    m_updated = true;
                }
            }
            else
            {
                LOGWARN("skipped IARM event %d for unknown device %d", eData->changedStatus, eData->logicalAddress);
            }
        }

        void HdmiCec::cecHostDeviceStatusChangedEventHandler(const char* owner_str, IARM_EventId_t eventId, void* data_ptr, size_t len)
        {
            LOGINFO("owner=%s", owner_str);

            if (!HdmiCec::_instance)
                return;

            if (data_ptr && owner_str
                    && (eventId == IARM_BUS_CECHost_EVENT_DEVICESTATUSCHANGE)
                    && (strcmp(owner_str, IARM_BUS_CECHOST_NAME) == 0))
            {
                _instance->onDeviceStatusChanged(eventId, data_ptr, len);
            }
        }

        void HdmiCec::onDeviceStatusUpdateEnd(IARM_EventId_t eventId, const void* data_ptr, size_t len)
        {
            const IARM_Bus_CECHost_DeviceStatusUpdateEnd_EventData_t* eData = static_cast<const IARM_Bus_CECHost_DeviceStatusUpdateEnd_EventData_t*>(data_ptr);

            LOGINFO("scanId %d(%d) scan finished %d", eData->scanId, m_scan_id.load(), eData->isScanFinished);

            if (eData->isScanFinished != 1)
            {
                LOGWARN("scan corrupted scanId %d(%d) scan finished %d", eData->scanId, m_scan_id.load(), eData->isScanFinished);
                return;
            }

            if ((m_scan_id > 0) && (m_scan_id != eData->scanId))
            {
                LOGWARN("skipped on invalid scan_id %d(%d) scan finished %d", eData->scanId, m_scan_id.load(), eData->isScanFinished);
            }
            else
            {
                if (m_updated)
                {
                    LOGWARN("Update OK, finished (%d) scanId %d(%d)", eData->isScanFinished, eData->scanId, m_scan_id.load());
                    m_updated = false;
                    {
                        std::lock_guard<std::mutex> guard(m_mutex);
                        m_devices = m_scan_devices;
                        m_rescan_in_progress = false;
                    }
                    onDevicesChanged();
                }
                else
                {
                    m_rescan_in_progress = false;
                    LOGWARN("skipped: no changes on devices (empty callback)");
                }
            }
            m_scan_id = 0;
        }

        void HdmiCec::cecHostDeviceStatusUpdateEndEventHandler(const char* owner_str, IARM_EventId_t eventId, void* data_ptr, size_t len)
        {
            LOGINFO();

            if (!HdmiCec::_instance)
                return;

            if (data_ptr && owner_str
                    && (strcmp(owner_str, IARM_BUS_CECHOST_NAME) == 0))
            {
                _instance->onDeviceStatusUpdateEnd(eventId, data_ptr, len);
            }
        }

        bool HdmiCec::setOSDName(const char* name, int logical_address)
        {
            bool result = false;

            LOGINFO("OSD name: '%s' set for device %d", name, logical_address);

            if ((logical_address >= LogicalAddress::TV) && (logical_address < LogicalAddress::UNREGISTERED))
            {
                string dev_type = LogicalAddress(logical_address).toString();
                std::lock_guard<std::mutex> guard(m_mutex);
                device_t& device = m_scan_devices[dev_type];

                result = (device.osdName!= name);
                device.osdName = name;
                LOGINFO("CEC: OSD NAME:'%s' set for device %s", name, dev_type.c_str());
            }
            else
            {
                LOGWARN("CEC: device osdName failed - invalid logical address %d", logical_address);
            }
            return result;
        }

        bool HdmiCec::setVendorId(uint32_t vendor_id, int logical_address)
        {
            bool result = false;

            LOGWARN("VendorID: '%u' set for device %d", vendor_id, logical_address);

            string dev_type = LogicalAddress(logical_address).toString();
            std::lock_guard<std::mutex> guard(m_mutex);
            device_t& device = m_scan_devices[dev_type];
            std::ostringstream vendor_id_conv;
            vendor_id_conv << std::hex << vendor_id;
            string vendor = vendor_id_conv.str();

            if (device.vendor_id != vendor)
            {
                device.vendor_id = vendor;
                LOGWARN("set vendor id %s for device %d", device.vendor_id.c_str(), logical_address);
            }
            else
            {
                LOGINFO("skipped device update (the same vendor id 0x%x) for device %d", vendor_id, logical_address);
            }

            return result;
        }

        bool HdmiCec::setConnectedState(int connected, int logical_address)
        {
            bool result = false;

            LOGINFO("connected: %d set for device %d", connected, logical_address);

            string dev_type = LogicalAddress(logical_address).toString();
            std::lock_guard<std::mutex> guard(m_mutex);
            device_t& device = m_scan_devices[dev_type];

            result = device.connected != ((connected != 0) ? true : false);
            if (result)
            {
                device.connected = (connected != 0);
                LOGWARN("set connected %d set for %s", connected, dev_type.c_str());
            }

            return result;
        }

        bool HdmiCec::setPowerState(int power_state, int logical_address)
        {
            bool result = false;

            LOGINFO("power_state: %d set for device %d", power_state, logical_address);

            string dev_type = LogicalAddress(logical_address).toString();
            std::lock_guard<std::mutex> guard(m_mutex);
            device_t& device = m_scan_devices[dev_type];

            result = (device.power_state != ((power_state != 0) ? true : false));

            if (result)
            {
                device.power_state = (power_state != 0);
                LOGWARN("set power_state %d for %s", power_state, dev_type.c_str());
            }

            return result;
        }

	bool HdmiCec::setSystemAudioMode(int system_audio_mode)
	{
	    LOGINFO("system_audio_mode: %d\n", system_audio_mode);

	    bool old_value = m_system_audio_mode;
	    m_system_audio_mode = (system_audio_mode != 0);

	    return old_value != m_system_audio_mode;
	}

    } // namespace Plugin
} // namespace WPEFramework



