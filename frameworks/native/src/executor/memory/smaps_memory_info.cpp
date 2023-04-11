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
#include "executor/memory/smaps_memory_info.h"

#include <cinttypes>
#include <fstream>
#include <numeric>
#include <thread>
#include <v1_0/imemory_tracker_interface.h>
#include <cstring>

#include "dump_common_utils.h"
#include "executor/memory/get_cma_info.h"
#include "executor/memory/get_hardware_info.h"
#include "executor/memory/get_kernel_info.h"
#include "executor/memory/get_process_info.h"
#include "executor/memory/get_ram_info.h"
#include "executor/memory/memory_util.h"
#include "executor/memory/parse/meminfo_data.h"
#include "executor/memory/parse/parse_meminfo.h"
#include "executor/memory/parse/parse_smaps_rollup_info.h"
#include "executor/memory/parse/parse_smaps_info.h"
#include "hdf_base.h"
#include "hilog_wrapper.h"
#include "mem_mgr_constant.h"
#include "securec.h"
#include "string_ex.h"
#include "util/string_utils.h"

using namespace std;
using namespace OHOS::HDI::Memorytracker::V1_0;
namespace OHOS {
namespace HiviewDFX {
static const int LINE_WIDTH = 14;
static const int LINE_NAME_VAL_WIDTH = 60;
static const int LINE_NAME_KEY_WIDTH = 28;
static const size_t TYPE_SIZE = 2;
static const char SEPARATOR = '-';
static const char BLANK = ' ';
SmapsMemoryInfo::SmapsMemoryInfo()
{
    sMapsMethodVec_.clear();
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_RSS,
        bind(&SmapsMemoryInfo::SetRss, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_PSS,
        bind(&SmapsMemoryInfo::SetPss, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_SHARED_CLEAN,
        bind(&SmapsMemoryInfo::SetSharedClean, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_SHARED_DIRTY,
        bind(&SmapsMemoryInfo::SetSharedDirty, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_PRIVATE_CLEAN,
        bind(&SmapsMemoryInfo::SetPrivateClean, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_PRIVATE_DIRTY,
        bind(&SmapsMemoryInfo::SetPrivateDirty, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_SWAP,
        bind(&SmapsMemoryInfo::SetSwap, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_SWAP_PSS,
        bind(&SmapsMemoryInfo::SetSwapPss, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_SIZE,
        bind(&SmapsMemoryInfo::SetSize, this, placeholders::_1, placeholders::_2)));
    sMapsMethodVec_.push_back(make_pair(SMAPS_MEMINFO_COUNTS,
        bind(&SmapsMemoryInfo::SetCounts, this, placeholders::_1, placeholders::_2)));
}

SmapsMemoryInfo::~SmapsMemoryInfo()
{
}

void SmapsMemoryInfo::InsertSmapsTitle(StringMatrix result)
{
    vector<string> line1;
    vector<string> line2;
    vector<string> line3;
    string space = " ";
    StringUtils::GetInstance().SetWidth(LINE_WIDTH, BLANK, true, space);

    string separator = "-";
    StringUtils::GetInstance().SetWidth(LINE_WIDTH, SEPARATOR, true, separator);

    string unit = "(" + MemoryUtil::GetInstance().KB_UNIT_ + " )";
    StringUtils::GetInstance().SetWidth(LINE_WIDTH, BLANK, true, unit);

    // Add  spaces at the beginning of the line
    line1.push_back(space);
    line2.push_back(space);
    line3.push_back(space);

    for (string str : MemoryFilter::GetInstance().TITLE_SMAPS_HAS_PID_) {
        vector<string> types;
        StringUtils::GetInstance().StringSplit(str, "_", types);
        if (types.size() == TYPE_SIZE) {
            string title1 = types.at(0);
            StringUtils::GetInstance().SetWidth(LINE_WIDTH, BLANK, true, title1);
            line1.push_back(title1);
            string title2 = types.at(1);
            StringUtils::GetInstance().SetWidth(LINE_WIDTH, BLANK, true, title2);
            line2.push_back(title2);
            line3.push_back(separator);
        } else {
            string title = types.at(0);
            StringUtils::GetInstance().SetWidth(StringUtils::GetInstance().IsSameStr(title, "Name") ?
                LINE_NAME_KEY_WIDTH : LINE_WIDTH, BLANK, true, title);
            line1.push_back(space);
            line2.push_back(title);
            title = TrimStr(title);
            line3.push_back(separator);
    }
    }
    result->push_back(line1);
    result->push_back(line2);
    result->push_back(line3);
}

void SmapsMemoryInfo::BuildSmapsResult(const GroupMap &infos, StringMatrix result)
{
    InsertSmapsTitle(result);
    for (const auto &info : infos) {
        vector<string> tempResult;
        string space = " ";
        StringUtils::GetInstance().SetWidth(LINE_WIDTH, BLANK, false, space);
        auto &valueMap = info.second;
        for (const auto &tag : MemoryFilter::GetInstance().VALUE_SMAPS_WITH_PID) {
            auto it = valueMap.find(tag);
            string value = "0";
            if (it != valueMap.end()) {
                value = StringUtils::GetInstance().IsSameStr(tag, "Name") ? info.first : to_string(it->second);
            }
            if (StringUtils::GetInstance().IsSameStr(tag, "Name")) {
                DUMPER_HILOGI(MODULE_SERVICE, "tag is Name");
                value = space + value;
                StringUtils::GetInstance().SetWidth(LINE_NAME_VAL_WIDTH, BLANK, true, value);
            } else {
                StringUtils::GetInstance().SetWidth(LINE_WIDTH, BLANK, false, value);
            }
            tempResult.push_back(value);
        }
        result->push_back(tempResult);
    }
}


void SmapsMemoryInfo::CalcSmapsGroup(const GroupMap &infos, StringMatrix result,
                                     MemInfoData::MemSmapsInfo &memSmapsInfo)
{
    for (const auto &info : infos) {
        auto &valueMap = info.second;
        for (const auto &method : sMapsMethodVec_) {
            auto it = valueMap.find(method.first);
            if (it != valueMap.end()) {
                method.second(memSmapsInfo, it->second);
            }
        }
    }
    vector<string> lines;
    vector<string> values;
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.size), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.rss), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.pss), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.sharedClean), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.sharedDirty), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.privateClean), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.privateDirty), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.swap), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.swapPss), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue(to_string(memSmapsInfo.counts), lines, values);
    MemoryUtil::GetInstance().SetMemTotalValue("Summary", lines, values);

    result->push_back(lines);
    result->push_back(values);
}

bool SmapsMemoryInfo::ShowMemorySmapsByPid(const int &pid, StringMatrix result)
{
    DUMPER_HILOGI(MODULE_SERVICE, "GetMemoryInfoByPid");
    GroupMap groupMap;
    MemInfoData::MemSmapsInfo memSmapsinfo;
    MemoryUtil::GetInstance().InitMemSmapsInfo(memSmapsinfo);
    unique_ptr<ParseSmapsInfo> parseSmapsInfo = make_unique<ParseSmapsInfo>();
    if (!parseSmapsInfo->ShowSmapsData(MemoryFilter::APPOINT_PID, pid, groupMap, memSmapsinfo)) {
        DUMPER_HILOGE(MODULE_SERVICE, "parse smaps info fail");
        return false;
    }

    MemInfoData::GraphicsMemory graphicsMemory;
    MemoryUtil::GetInstance().InitGraphicsMemory(graphicsMemory);
    BuildSmapsResult(groupMap, result);
    CalcSmapsGroup(groupMap, result, memSmapsinfo);
    return true;
}

void SmapsMemoryInfo::SetPss(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.pss += value;
}

void SmapsMemoryInfo::SetRss(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.rss += value;
}

void SmapsMemoryInfo::SetSharedClean(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.sharedClean += value;
}

void SmapsMemoryInfo::SetSharedDirty(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.sharedDirty += value;
}

void SmapsMemoryInfo::SetPrivateClean(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.privateClean += value;
}

void SmapsMemoryInfo::SetPrivateDirty(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.privateDirty += value;
}

void SmapsMemoryInfo::SetSwap(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.swap += value;
}

void SmapsMemoryInfo::SetSwapPss(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.swapPss += value;
}

void SmapsMemoryInfo::SetSize(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.size += value;
}

void SmapsMemoryInfo::SetName(MemInfoData::MemSmapsInfo &meminfo, const string &value)
{
    meminfo.name = value;
}

void SmapsMemoryInfo::SetCounts(MemInfoData::MemSmapsInfo &meminfo, uint64_t value)
{
    meminfo.counts += value;
}
} // namespace HiviewDFX
} // namespace OHOS