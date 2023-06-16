/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "dump_manager_cpu_service.h"
#include <file_ex.h>
#include <if_system_ability_manager.h>
#include <ipc_skeleton.h>
#include <iservice_registry.h>
#include <string_ex.h>
#include <system_ability_definition.h>
#include <thread>
#include <unistd.h>
#include "securec.h"

#include "common/dumper_constant.h"
#include "common_event_data.h"
#include "common_event_manager.h"
#include "common_event_support.h"
#include "dump_log_manager.h"
#include "hilog_wrapper.h"
#include "inner/dump_service_id.h"
#include "manager/dump_implement.h"
#include "dump_utils.h"
#include "util/string_utils.h"
#ifdef HIDUMPER_ABILITY_BASE_ENABLE
#include "dump_app_state_observer.h"
#endif
#ifdef HIDUMPER_BATTERY_ENABLE
#include "dump_battery_stats_subscriber.h"
#endif

using namespace std;
namespace OHOS {
namespace HiviewDFX {
namespace {
const std::string DUMPMGR_CPU_SERVICE_NAME = "HiDumperCpuService";
auto dumpManagerCpuService = DumpDelayedSpSingleton<DumpManagerCpuService>::GetInstance();
static const std::string LOAD_AVG_FILE_PATH = "/proc/loadavg";
static const size_t LOAD_AVG_INFO_COUNT = 3;
static const int PROC_CPU_LENGTH = 256;
static const long unsigned HUNDRED_PERCENT_VALUE = 100;
static const long unsigned DELAY_VALUE = 500000;
}
DumpManagerCpuService::DumpManagerCpuService() : SystemAbility(DFX_SYS_HIDUMPER_CPU_ABILITY_ID, true)
{
}

DumpManagerCpuService::~DumpManagerCpuService()
{
    delete sysAbilityListener_;
}

void DumpManagerCpuService::OnStart()
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "on start");
    if (started_) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "it's ready, nothing to do.");
        return;
    }

    DUMPER_HILOGI(MODULE_CPU_SERVICE, "cpu service OnStart");
    if (!Init()) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "init fail, nothing to do.");
        return;
    }

    started_ = true;
}

void DumpManagerCpuService::OnStop()
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "cpu service onStop");
    if (!started_) {
        DUMPER_HILOGI(MODULE_CPU_SERVICE, "is not ready, nothing to do");
        return;
    }
    ResetParam();

#ifdef HIDUMPER_ABILITY_BASE_ENABLE
    RemoveSystemAbilityListener(APP_MGR_SERVICE_ID);
    DumpAppStateObserver::GetInstance().UnsubscribeAppState();
#endif
#ifdef HIDUMPER_BATTERY_ENABLE
    RemoveSystemAbilityListener(COMMON_EVENT_SERVICE_ID);
    if (!OHOS::EventFwk::CommonEventManager::UnSubscribeCommonEvent(subscriberPtr_)) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "unregister to commonevent manager failed");
    }
#endif

    DUMPER_HILOGI(MODULE_CPU_SERVICE, "cpu service leave");
}

int32_t DumpManagerCpuService::Request(DumpCpuData &dumpCpuData)
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "enter");
    InitParam(dumpCpuData);
    int32_t ret = DumpCpuUsageData();
    dumpCpuData.dumpCPUDatas_ = *dumpCPUDatas_;
    return ret;
}

std::shared_ptr<DumpEventHandler> DumpManagerCpuService::GetHandler()
{
    if (handler_ == nullptr) {
        DUMPER_HILOGI(MODULE_CPU_SERVICE, "init handler at get handler");
        handler_ = std::make_shared<DumpEventHandler>(eventRunner_, dumpManagerCpuService);
    }
    return handler_;
}

bool DumpManagerCpuService::Init()
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "init");
    if (sysAbilityListener_ != nullptr) {
        return false;
    }

    sysAbilityListener_ = new (std::nothrow) SystemAbilityStatusChangeListener();
    if (sysAbilityListener_ == nullptr) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "Failed to create statusChangeListener due to no memory");
        return false;
    }

    sptr<ISystemAbilityManager> systemAbilityManager
        = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (systemAbilityManager == nullptr) {
        delete sysAbilityListener_;
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "systemAbilityManager is null");
        return false;
    }
#ifdef HIDUMPER_ABILITY_BASE_ENABLE
    if (systemAbilityManager->SubscribeSystemAbility(APP_MGR_SERVICE_ID, sysAbilityListener_) != ERR_OK) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "subscribe system ability id: %{public}d failed", APP_MGR_SERVICE_ID);
    }
#endif
#ifdef HIDUMPER_BATTERY_ENABLE
    if (systemAbilityManager->SubscribeSystemAbility(COMMON_EVENT_SERVICE_ID, sysAbilityListener_) != ERR_OK) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "subscribe system ability id: %{public}d failed",
            COMMON_EVENT_SERVICE_ID);
    }
#endif

    return true;
}

void DumpManagerCpuService::EventHandlerInit()
{
    if (dumpManagerCpuService->registered_) {
        return;
    }
    if (dumpManagerCpuService->eventRunner_ == nullptr) {
        dumpManagerCpuService->eventRunner_ = AppExecFwk::EventRunner::Create(DUMPMGR_CPU_SERVICE_NAME);
        if (dumpManagerCpuService->eventRunner_ == nullptr) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "create EventRunner");
            return;
        }
    }
    dumpManagerCpuService->eventRunner_->Run();

    if (dumpManagerCpuService->handler_ == nullptr) {
        DUMPER_HILOGI(MODULE_CPU_SERVICE, "init handler at init");
        dumpManagerCpuService->handler_ = std::make_shared<DumpEventHandler>(dumpManagerCpuService->eventRunner_,
            dumpManagerCpuService);
    }
    dumpManagerCpuService->handler_->SendEvent(DumpEventHandler::MSG_GET_CPU_INFO_ID,
        DumpEventHandler::GET_CPU_INFO_DELAY_TIME_INIT);

    dumpManagerCpuService->registered_ = true;
}

bool DumpManagerCpuService::SendImmediateEvent()
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "send immediate event");
    if (eventRunner_ == nullptr) {
        eventRunner_ = AppExecFwk::EventRunner::Create(DUMPMGR_CPU_SERVICE_NAME);
        if (eventRunner_ == nullptr) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "create EventRunner");
            return false;
        }
    }

    if (handler_ == nullptr) {
        handler_ = std::make_shared<DumpEventHandler>(eventRunner_, dumpManagerCpuService);
    }

    handler_->SendImmediateEvent(DumpEventHandler::MSG_GET_CPU_INFO_ID);
    return true;
}

void DumpManagerCpuService::SystemAbilityStatusChangeListener::OnAddSystemAbility(
    int32_t systemAbilityId, const std::string& deviceId)
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "systemAbilityId=%{public}d, deviceId=%{private}s", systemAbilityId,
        deviceId.c_str());

#ifdef HIDUMPER_ABILITY_BASE_ENABLE
    if (systemAbilityId == APP_MGR_SERVICE_ID) {
        dumpManagerCpuService->SubscribeAppStateEvent();
    }
#endif
#ifdef HIDUMPER_BATTERY_ENABLE
    if (systemAbilityId == COMMON_EVENT_SERVICE_ID) {
        dumpManagerCpuService->SubscribeCommonEvent();
    }
#endif

    dumpManagerCpuService->EventHandlerInit();
}

void DumpManagerCpuService::SystemAbilityStatusChangeListener::OnRemoveSystemAbility(
    int32_t systemAbilityId, const std::string& deviceId)
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "Remove system ability, system ability id");
}

bool DumpManagerCpuService::SubscribeAppStateEvent()
{
    return DumpAppStateObserver::GetInstance().SubscribeAppState();
}

bool DumpManagerCpuService::SubscribeCommonEvent()
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "Subscribe CommonEvent enter");
    bool result = false;
    OHOS::EventFwk::MatchingSkills matchingSkills;
    matchingSkills.AddEvent(OHOS::EventFwk::CommonEventSupport::COMMON_EVENT_BATTERY_CHANGED);
    OHOS::EventFwk::CommonEventSubscribeInfo subscribeInfo(matchingSkills);
    if (!subscriberPtr_) {
        subscriberPtr_ = std::make_shared<DumpBatteryStatsSubscriber>(subscribeInfo);
    }
    result = OHOS::EventFwk::CommonEventManager::SubscribeCommonEvent(subscriberPtr_);
    if (!result) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "Subscribe CommonEvent failed");
    } else {
        DUMPER_HILOGI(MODULE_CPU_SERVICE, "Subscribe CommonEvent success");
    }
    return result;
}

void DumpManagerCpuService::InitParam(DumpCpuData &dumpCpuData)
{
    cpuUsagePid_ = dumpCpuData.cpuUsagePid_;
    if (cpuUsagePid_ != -1) {
        curSpecProc_ = std::make_shared<ProcInfo>();
        oldSpecProc_ = std::make_shared<ProcInfo>();
    }
    curCPUInfo_ = std::make_shared<CPUInfo>();
    oldCPUInfo_ = std::make_shared<CPUInfo>();
    dumpCPUDatas_ = std::make_shared<std::vector<std::vector<std::string>>>(dumpCpuData.dumpCPUDatas_);
}

void DumpManagerCpuService::ResetParam()
{
    curCPUInfo_.reset();
    oldCPUInfo_.reset();
    curProcs_.clear();
    oldProcs_.clear();
    if (cpuUsagePid_ != -1) {
        curSpecProc_.reset();
        oldSpecProc_.reset();
    }
}

int DumpManagerCpuService::DumpCpuUsageData()
{
    if (!DumpCpuInfoUtil::GetInstance().GetCurCPUInfo(curCPUInfo_)) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get current cpu info failed!.");
        return DumpStatus::DUMP_FAIL;
    }
    if (!DumpCpuInfoUtil::GetInstance().GetOldCPUInfo(oldCPUInfo_)) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get old cpu info failed!.");
        return DumpStatus::DUMP_FAIL;
    }

    if (cpuUsagePid_ != -1) {
        if (!GetProcCPUInfo()) {
            return DumpStatus::DUMP_FAIL;
        }
    } else {
        if (!DumpCpuInfoUtil::GetInstance().GetCurProcInfo(curProcs_)) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get current process info failed!.");
            return DumpStatus::DUMP_FAIL;
        }
        if (!DumpCpuInfoUtil::GetInstance().GetOldProcInfo(oldProcs_)) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get old process info failed!.");
            return DumpStatus::DUMP_FAIL;
        }
    }
    std::string avgInfo;
    DumpStatus ret = ReadLoadAvgInfo(LOAD_AVG_FILE_PATH, avgInfo);
    if (ret != DumpStatus::DUMP_OK) {
        DUMPER_HILOGD(MODULE_CPU_SERVICE, "Get LoadAvgInfo failed!");
        return DumpStatus::DUMP_FAIL;
    }
    AddStrLineToDumpInfo(avgInfo);

    DumpCpuInfoUtil::GetInstance().GetUpdateCpuStartTime(startTime_);
    DumpCpuInfoUtil::GetInstance().GetDateAndTime(endTime_);
    std::string dumpTimeStr;
    CreateDumpTimeString(startTime_, endTime_, dumpTimeStr);
    AddStrLineToDumpInfo(dumpTimeStr);

    std::string cpuStatStr;
    CreateCPUStatString(cpuStatStr);
    AddStrLineToDumpInfo(cpuStatStr);

    DumpProcInfo();
    DumpCpuInfoUtil::GetInstance().UpdateCpuInfo();
    return DumpStatus::DUMP_OK;
}

bool DumpManagerCpuService::GetProcCPUInfo()
{
    bool ret = false;
    if (!DumpCpuInfoUtil::GetInstance().GetOldSpecProcInfo(cpuUsagePid_, oldSpecProc_)) {
        DumpCpuInfoUtil::GetInstance().UpdateCpuInfo();
        if (!DumpCpuInfoUtil::GetInstance().GetOldSpecProcInfo(cpuUsagePid_, oldSpecProc_)) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get old process %{public}d info failed!.", cpuUsagePid_);
            return ret;
        }

        DumpCpuInfoUtil::GetInstance().CopyCpuInfo(oldCPUInfo_, curCPUInfo_);
        usleep(DELAY_VALUE);
        if (!DumpCpuInfoUtil::GetInstance().GetCurCPUInfo(curCPUInfo_)) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get current cpu info failed!.");
            return ret;
        }
    }

    if (!DumpCpuInfoUtil::GetInstance().GetCurSpecProcInfo(cpuUsagePid_, curSpecProc_)) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "Get current process %{public}d info failed!.", cpuUsagePid_);
        return ret;
    }
    ret = true;
    return ret;
}

DumpStatus DumpManagerCpuService::ReadLoadAvgInfo(const std::string &filePath, std::string &info)
{
    if (!FileExists(filePath)) {
        return DumpStatus::DUMP_FAIL;
    }

    std::string rawData;
    if (!LoadStringFromFile(filePath, rawData)) {
        return DumpStatus::DUMP_FAIL;
    }
    DUMPER_HILOGD(MODULE_CPU_SERVICE, "rawData is %{public}s", rawData.c_str());
    std::vector<std::string> vec;
    SplitStr(rawData, DumpUtils::SPACE, vec);
    info = "Load average:";
    for (size_t i = 0; i < LOAD_AVG_INFO_COUNT; i++) {
        info.append(" ");
        info.append(vec[i]);
        if (i == LOAD_AVG_INFO_COUNT - 1) {
            info.append(";");
        } else {
            info.append(" /");
        }
    }
    info.append(" the cpu load average in 1 min, 5 min and 15 min");
    DUMPER_HILOGD(MODULE_CPU_SERVICE, "info is %{public}s", info.c_str());
    return DumpStatus::DUMP_OK;
}

void DumpManagerCpuService::CreateDumpTimeString(const std::string &startTime,
    const std::string &endTime, std::string &timeStr)
{
    DUMPER_HILOGD(MODULE_CPU_SERVICE, "start:%{public}s, end:%{public}s", startTime.c_str(), endTime.c_str());
    timeStr = "CPU usage from";
    timeStr.append(startTime);
    timeStr.append(" to");
    timeStr.append(endTime);
}

void DumpManagerCpuService::AddStrLineToDumpInfo(const std::string &strLine)
{
    std::vector<std::string> vec;
    vec.push_back(strLine);
    dumpCPUDatas_->push_back(vec);
}

void DumpManagerCpuService::CreateCPUStatString(std::string &str)
{
    long unsigned totalDeltaTime = (curCPUInfo_->uTime + curCPUInfo_->nTime + curCPUInfo_->sTime + curCPUInfo_->iTime
                                    + curCPUInfo_->iowTime + curCPUInfo_->irqTime + curCPUInfo_->sirqTime)
                                   - (oldCPUInfo_->uTime + oldCPUInfo_->nTime + oldCPUInfo_->sTime + oldCPUInfo_->iTime
                                      + oldCPUInfo_->iowTime + oldCPUInfo_->irqTime + oldCPUInfo_->sirqTime);
    if (cpuUsagePid_ != -1) {
        curSpecProc_->userSpaceUsage =
            (curSpecProc_->uTime - oldSpecProc_->uTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
        curSpecProc_->sysSpaceUsage =
            (curSpecProc_->sTime - oldSpecProc_->sTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
        curSpecProc_->totalUsage = curSpecProc_->userSpaceUsage + curSpecProc_->sysSpaceUsage;
    } else {
        for (size_t i = 0; i < curProcs_.size(); i++) {
            if (curProcs_[i] == nullptr) {
                continue;
            }
            std::shared_ptr<ProcInfo> oldProc = GetOldProc(curProcs_[i]->pid);
            if (oldProc) {
                curProcs_[i]->userSpaceUsage =
                    (curProcs_[i]->uTime - oldProc->uTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
                curProcs_[i]->sysSpaceUsage =
                    (curProcs_[i]->sTime - oldProc->sTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
                curProcs_[i]->totalUsage = curProcs_[i]->userSpaceUsage + curProcs_[i]->sysSpaceUsage;
            } else {
                curProcs_[i]->userSpaceUsage = 0;
                curProcs_[i]->sysSpaceUsage = 0;
                curProcs_[i]->totalUsage = 0;
            }
        }
    }

    long unsigned userSpaceUsage =
        ((curCPUInfo_->uTime + curCPUInfo_->nTime) - (oldCPUInfo_->uTime + oldCPUInfo_->nTime)) * HUNDRED_PERCENT_VALUE
        / totalDeltaTime;
    long unsigned sysSpaceUsage = (curCPUInfo_->sTime - oldCPUInfo_->sTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
    long unsigned iowUsage = (curCPUInfo_->iowTime - oldCPUInfo_->iowTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
    long unsigned irqUsage =
        ((curCPUInfo_->irqTime + curCPUInfo_->sirqTime) - (oldCPUInfo_->irqTime + oldCPUInfo_->sirqTime))
        * HUNDRED_PERCENT_VALUE / totalDeltaTime;
    long unsigned idleUsage = (curCPUInfo_->iTime - oldCPUInfo_->iTime) * HUNDRED_PERCENT_VALUE / totalDeltaTime;
    long unsigned totalUsage = userSpaceUsage + sysSpaceUsage;

    str = "Total: ";
    str.append(std::to_string(totalUsage)).append("%; ");
    str.append("User Space: ").append(std::to_string(userSpaceUsage)).append("%; ");
    str.append("Kernel Space: ").append(std::to_string(sysSpaceUsage)).append("%; ");
    str.append("iowait: ").append(std::to_string(iowUsage)).append("%; ");
    str.append("irq: ").append(std::to_string(irqUsage)).append("%; ");
    str.append("idle: ").append(std::to_string(idleUsage)).append("%");
}

std::shared_ptr<ProcInfo> DumpManagerCpuService::GetOldProc(const std::string &pid)
{
    for (size_t i = 0; i < oldProcs_.size(); i++) {
        if (StringUtils::GetInstance().IsSameStr(oldProcs_[i]->pid, pid)) {
            return oldProcs_[i];
        }
    }
    return nullptr;
}

void DumpManagerCpuService::DumpProcInfo()
{
    std::vector<std::shared_ptr<ProcInfo>> sortedInfos;
    sortedInfos.assign(curProcs_.begin(), curProcs_.end());
    std::sort(sortedInfos.begin(), sortedInfos.end(), SortProcInfo);

    AddStrLineToDumpInfo("Details of Processes:");
    AddStrLineToDumpInfo("    PID   Total Usage	   User Space    Kernel Space    Page Fault Minor"
                         "    Page Fault Major    Name");
    if (cpuUsagePid_ != -1) {
        char format[PROC_CPU_LENGTH] = {0};
        int ret = sprintf_s(format, PROC_CPU_LENGTH,
                            "    %-5s    %3lu%%             %3lu%%"
                            "           %3lu%%            %8s            %8s        %-15s",
                            (curSpecProc_->pid).c_str(), curSpecProc_->totalUsage,
                            curSpecProc_->userSpaceUsage, curSpecProc_->sysSpaceUsage,
                            (curSpecProc_->minflt).c_str(), (curSpecProc_->majflt).c_str(),
                            (curSpecProc_->comm).c_str());
        AddStrLineToDumpInfo(std::string(format));
        if (ret < 0) {
            DUMPER_HILOGE(MODULE_CPU_SERVICE, "Dump process %{public}d cpu info failed!.", cpuUsagePid_);
        }
        return;
    }
    for (size_t i = 0; i < sortedInfos.size(); i++) {
        char format[PROC_CPU_LENGTH] = {0};
        int ret = sprintf_s(format, PROC_CPU_LENGTH,
                            "    %-5s    %3lu%%             %3lu%%"
                            "           %3lu%%            %8s            %8s        %-15s",
                            (sortedInfos[i]->pid).c_str(), sortedInfos[i]->totalUsage,
                            sortedInfos[i]->userSpaceUsage, sortedInfos[i]->sysSpaceUsage,
                            (sortedInfos[i]->minflt).c_str(), (sortedInfos[i]->majflt).c_str(),
                            (sortedInfos[i]->comm).c_str());
        if (ret < 0) {
            continue;
        }
        AddStrLineToDumpInfo(std::string(format));
    }
}

bool DumpManagerCpuService::SortProcInfo(std::shared_ptr<ProcInfo> &left, std::shared_ptr<ProcInfo> &right)
{
    if (right->totalUsage != left->totalUsage) {
        return right->totalUsage < left->totalUsage;
    }
    if (right->userSpaceUsage != left->userSpaceUsage) {
        return right->userSpaceUsage < left->userSpaceUsage;
    }
    if (right->sysSpaceUsage != left->sysSpaceUsage) {
        return right->sysSpaceUsage < left->sysSpaceUsage;
    }
    if (right->pid.length() != left->pid.length()) {
        return right->pid.length() < left->pid.length();
    }
    return (right->pid.compare(left->pid) < 0);
}

void DumpManagerCpuService::StartService()
{
    DUMPER_HILOGI(MODULE_CPU_SERVICE, "dumper StartService");
    sptr<ISystemAbilityManager> samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr == nullptr) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "failed to find SystemAbilityManager.");
        return;
    }
    if (dumpManagerCpuService == nullptr) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "DumpManagerCpuService service is null.");
        return;
    }
    int ret = samgr->AddSystemAbility(DFX_SYS_HIDUMPER_CPU_ABILITY_ID, dumpManagerCpuService);
    if (ret != 0) {
        DUMPER_HILOGE(MODULE_CPU_SERVICE, "failed to add sys dump cpu service ability.");
        return;
    }
    OnStart();
}
} // namespace HiviewDFX
} // namespace OHOS
